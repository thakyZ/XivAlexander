﻿#pragma once

#include "IFixedSizeFont.h"

#include "../Internal/BitmapCopy.h"
#include "../Internal/ThreadPool.h"

#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "../Internal/TeamHypersomnia-rectpack2D/src/finders_interface.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

namespace XivRes::FontGenerator {
	class FontdataPacker {
		size_t m_nThreads = std::thread::hardware_concurrency();
		int m_nSideLength = 1024;
		int m_nHorizontalOffset = 0;
		std::vector<std::shared_ptr<IFixedSizeFont>> m_fonts;

	public:
		size_t AddFont(std::shared_ptr<IFixedSizeFont> font) {
			m_fonts.emplace_back(std::move(font));
			return m_fonts.size() - 1;
		}

		std::shared_ptr<IFixedSizeFont> GetFont(size_t index) const {
			return m_fonts.at(index);
		}

		void SetHorizontalOffset(int horizontalOffset) {
			m_nHorizontalOffset = horizontalOffset;
		}

		auto Compile() {
			using namespace rectpack2D;
			using spaces_type = rectpack2D::empty_spaces<false, default_empty_spaces>;
			using rect_type = output_rect_t<spaces_type>;

			struct CharacterPlan {
				size_t SourceFontIndex{};
				std::vector<FontdataStream*> TargetFonts{};
				FontdataGlyphEntry Entry{};
				const Internal::UnicodeBlocks::BlockDefinition* UnicodeBlock{};
				int X1{};
				int X2{};
			};

			std::vector<std::shared_ptr<FontdataStream>> targetFonts;
			targetFonts.resize(m_fonts.size());

			std::vector<std::vector<std::shared_ptr<IFixedSizeFont>>> threadSafeSourceFonts;
			threadSafeSourceFonts.reserve(m_fonts.size());
			size_t nMaxCharacterCount = 0;
			for (const auto& font : m_fonts) {
				nMaxCharacterCount += font->GetCodepointCount();
				threadSafeSourceFonts.emplace_back();
				threadSafeSourceFonts.back().resize(m_nThreads);
				threadSafeSourceFonts.back()[0] = font;
			}

			const auto GetThreadSafeSourceFont = [&threadSafeSourceFonts](size_t sourceFontIndex, size_t threadIndex) -> const IFixedSizeFont& {
				auto& copy = threadSafeSourceFonts[sourceFontIndex][threadIndex];
				if (!copy)
					copy = threadSafeSourceFonts[sourceFontIndex][0]->GetThreadSafeView();
				return *copy;
			};

			std::vector<CharacterPlan> rectangleInfoList;
			rectangleInfoList.reserve(nMaxCharacterCount);
			{
				std::vector<std::set<char32_t>> codepointsPerFont;
				codepointsPerFont.resize(m_fonts.size());

				{
					Internal::ThreadPool pool(m_nThreads);

					for (size_t i = 0; i < m_fonts.size(); i++) {
						auto& targetFont = *(targetFonts[i] = std::make_shared<FontdataStream>());

						pool.Submit([this, i, &targetFont, &codepointsPerFont, &GetThreadSafeSourceFont](size_t nThreadIndex) {
							const auto& font = GetThreadSafeSourceFont(i, nThreadIndex);

							targetFont.TextureWidth(m_nSideLength);
							targetFont.TextureHeight(m_nSideLength);
							targetFont.Size(font.GetSize());
							targetFont.LineHeight(font.GetLineHeight());
							targetFont.Ascent(font.GetAscent());

							codepointsPerFont[i] = font.GetAllCodepoints();
							targetFont.ReserveFontEntries(codepointsPerFont[i].size());
						});

						pool.Submit([&targetFont, i, &GetThreadSafeSourceFont](size_t nThreadIndex) {
							const auto& font = GetThreadSafeSourceFont(i, nThreadIndex);
							const auto pairs = font.GetKerningPairs();
							targetFont.ReserveKerningEntries(pairs.size());
							for (const auto& kerning : pairs) {
								if (kerning.second)
									targetFont.AddKerning(kerning.first.first, kerning.first.second, kerning.second);
							}
						});
					}

					pool.SubmitDoneAndWait();
				}

				std::map<const void*, CharacterPlan*> rectangleInfoMap;
				for (size_t i = 0; i < m_fonts.size(); i++) {
					const auto& font = m_fonts[i];
					for (const auto& codepoint : codepointsPerFont[i]) {
						auto& block = XivRes::Internal::UnicodeBlocks::GetCorrespondingBlock(codepoint);
						if (block.Flags & XivRes::Internal::UnicodeBlocks::RTL)
							continue;

						auto& pInfo = rectangleInfoMap[font->GetGlyphUniqid(codepoint)];
						if (!pInfo) {
							rectangleInfoList.emplace_back();
							pInfo = &rectangleInfoList.back();
							pInfo->SourceFontIndex = i;
							pInfo->UnicodeBlock = &block;
							pInfo->Entry.Char(codepoint);
						}
						pInfo->TargetFonts.emplace_back(targetFonts[i].get());
						targetFonts[i]->AddFontEntry(codepoint, 0, 0, 0, 0, 0, 0, 0);
					}
				}
			}

			{
				Internal::ThreadPool pool(m_nThreads);

				const auto divideUnit = (std::max<size_t>)(1, static_cast<size_t>(std::sqrt(static_cast<double>(rectangleInfoList.size()))));
				for (size_t nBase = 0; nBase < divideUnit; nBase++) {
					pool.Submit([divideUnit, &rectangleInfoList, &pool, nBase, &GetThreadSafeSourceFont](size_t nThreadIndex) {
						for (size_t i = nBase; i < rectangleInfoList.size(); i += divideUnit) {
							pool.AbortIfErrorOccurred();

							auto& info = rectangleInfoList[i];
							const auto& font = GetThreadSafeSourceFont(info.SourceFontIndex, nThreadIndex);

							GlyphMetrics gm;
							if (!font.GetGlyphMetrics(info.Entry.Char(), gm))
								throw std::runtime_error("Font reported to have a codepoint but it's failing to report glyph metrics");

							info.Entry.CurrentOffsetY = gm.Y1;
							info.Entry.BoundingHeight = Internal::RangeCheckedCast<uint8_t>(gm.Y2 - info.Entry.CurrentOffsetY);
							info.Entry.NextOffsetX = gm.AdvanceX;
							info.X1 = gm.X1;
							info.X2 = gm.X2;
						}
					});
				}

				pool.SubmitDoneAndWait();

				std::map<const Internal::UnicodeBlocks::BlockDefinition*, std::vector<CharacterPlan*>> excessiveHorizontalOffsetCharacters;
				for (auto& info : rectangleInfoList) {
					if (info.Entry.Char() == 3655)
						__debugbreak();

					if (info.X1 < -m_nHorizontalOffset) {
						info.Entry.BoundingWidth = Internal::RangeCheckedCast<uint8_t>(info.X2 - info.X1);
						info.X1 = -info.X1;
						info.X2 = info.X1 - m_nHorizontalOffset;
						info.Entry.NextOffsetX += info.X2 - info.Entry.BoundingWidth;
						excessiveHorizontalOffsetCharacters[info.UnicodeBlock].emplace_back(&info);
					} else {
						info.Entry.BoundingWidth = Internal::RangeCheckedCast<uint8_t>((std::max)(0, info.X2 + m_nHorizontalOffset));
						info.Entry.NextOffsetX -= info.Entry.BoundingWidth;
						info.X1 = m_nHorizontalOffset;
						info.X2 = 0;
					}
				}

				for (const auto& [pBlock, chars] : excessiveHorizontalOffsetCharacters) {
					if (strstr(pBlock->Name, "Thai"))
						__debugbreak();
					for (const auto& charPlan : chars) {
						auto entry = FontdataKerningEntry();
						entry.RightUtf8Value = charPlan->Entry.Utf8Value;
						entry.RightShiftJisValue = charPlan->Entry.ShiftJisValue;
						entry.RightOffset = -charPlan->X2;

						for (const auto& targetFont : charPlan->TargetFonts) {
							for (auto leftc = pBlock->First; leftc <= pBlock->Last; leftc++) {
								if (!targetFont->GetFontEntry(leftc))
									continue;

								entry.Left(leftc);
								targetFont->AddKerning(entry, true);
							}
						}
					}
				}

				// __debugbreak();
			}

			size_t nPlaneCount = 0;
			std::vector<rect_type> pendingRectangles;
			std::vector<CharacterPlan*> pendingPlans;
			std::vector<CharacterPlan*> successfulPlans, failedPlans;
			pendingRectangles.reserve(nMaxCharacterCount);
			pendingPlans.reserve(nMaxCharacterCount);
			failedPlans.reserve(nMaxCharacterCount);

			auto report_successful = [this, &successfulPlans, &pendingRectangles, &pendingPlans, &nPlaneCount](rect_type& r) {
				const auto index = &r - &pendingRectangles.front();
				const auto pInfo = pendingPlans[index];

				pInfo->Entry.TextureIndex = Internal::RangeCheckedCast<uint16_t>(nPlaneCount);
				pInfo->Entry.TextureOffsetX = Internal::RangeCheckedCast<uint16_t>(r.x + 1);
				pInfo->Entry.TextureOffsetY = Internal::RangeCheckedCast<uint16_t>(r.y + 1);

				for (auto& pFontdata : pInfo->TargetFonts)
					pFontdata->AddFontEntry(pInfo->Entry);

				successfulPlans.emplace_back(pInfo);

				return callback_result::CONTINUE_PACKING;
			};

			auto report_unsuccessful = [&failedPlans, &pendingRectangles, &pendingPlans](rect_type& r) {
				failedPlans.emplace_back(pendingPlans[&r - &pendingRectangles.front()]);
				return callback_result::CONTINUE_PACKING;
			};

			std::vector<std::shared_ptr<MemoryMipmapStream>> mipmapStreams;
			{
				Internal::ThreadPool pool(m_nThreads);

				for (auto& rectangleInfo : rectangleInfoList) {
					pendingRectangles.emplace_back(0, 0, rectangleInfo.Entry.BoundingWidth + 1, rectangleInfo.Entry.BoundingHeight + 1);
					pendingPlans.emplace_back(&rectangleInfo);
				}

				while (!pendingRectangles.empty()) {
					successfulPlans.reserve(nMaxCharacterCount);

					if (nPlaneCount == 0) {
						find_best_packing<spaces_type>(
							pendingRectangles,
							make_finder_input(
								m_nSideLength,
								1,
								report_successful,
								report_unsuccessful,
								flipping_option::DISABLED
							)
						);

					} else {
						// Already sorted from above
						find_best_packing_dont_sort<spaces_type>(
							pendingRectangles,
							make_finder_input(
								m_nSideLength,
								1,
								report_successful,
								report_unsuccessful,
								flipping_option::DISABLED
							)
						);
					}

					if (!successfulPlans.empty()) {
						for (size_t i = mipmapStreams.size(), i_ = (nPlaneCount + 4) / 4; i < i_; i++)
							mipmapStreams.emplace_back(std::make_shared<XivRes::MemoryMipmapStream>(m_nSideLength, m_nSideLength, 1, XivRes::TextureFormat::A8R8G8B8));
						const auto& pStream = mipmapStreams[nPlaneCount >> 2];
						const auto pCurrentTargetBuffer = &pStream->View<uint8_t>()[3 - (nPlaneCount & 3)];

						auto pSuccesses = std::make_shared<std::vector<CharacterPlan*>>(std::move(successfulPlans));
						successfulPlans = {};

						const auto divideUnit = (std::max<size_t>)(1, static_cast<size_t>(std::sqrt(static_cast<double>(pSuccesses->size()))));

						for (size_t nBase = 0; nBase < divideUnit; nBase++) {
							pool.Submit([divideUnit, pSuccesses, nBase, pCurrentTargetBuffer, w = pStream->Width, h = pStream->Height, &GetThreadSafeSourceFont](size_t nThreadIndex) {
								for (size_t i = nBase; i < pSuccesses->size(); i += divideUnit) {
									const auto pInfo = (*pSuccesses)[i];
									const auto& font = GetThreadSafeSourceFont(pInfo->SourceFontIndex, nThreadIndex);
									font.Draw(
										pInfo->Entry.Char(),
										pCurrentTargetBuffer,
										4,
										pInfo->Entry.TextureOffsetX + pInfo->X1,
										pInfo->Entry.TextureOffsetY - pInfo->Entry.CurrentOffsetY,
										w,
										h,
										255, 0, 255, 255
									);
								}
							});
						}
					}

					pendingRectangles.clear();
					pendingPlans.clear();
					for (const auto pInfo : failedPlans) {
						pendingRectangles.emplace_back(0, 0, pInfo->Entry.BoundingWidth + 1, pInfo->Entry.BoundingHeight + 1);
						pendingPlans.emplace_back(pInfo);
					}
					failedPlans.clear();
					nPlaneCount++;
				}

				pool.SubmitDoneAndWait();
			}

			return std::make_pair(targetFonts, mipmapStreams);
		}
	};
}
