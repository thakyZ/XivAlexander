#include "pch.h"
#include "Sqex_Sound_Writer.h"

Sqex::Sound::ScdWriter::SoundEntry Sqex::Sound::ScdWriter::SoundEntry::FromWave(const std::function<std::span<uint8_t>(size_t len, bool throwOnIncompleteRead)>& reader) {
	struct ExpectedFormat {
		LE<uint32_t> Riff;
		LE<uint32_t> RemainingSize;
		LE<uint32_t> Wave;
		LE<uint32_t> fmt_;
		LE<uint32_t> WaveFormatExSize;
	};
	const auto hdr = *reinterpret_cast<const ExpectedFormat*>(reader(sizeof ExpectedFormat, true).data());
	if (hdr.Riff != 0x46464952U || hdr.Wave != 0x45564157U || hdr.fmt_ != 0x20746D66U)
		throw std::invalid_argument("Bad file header");
		
	std::vector<uint8_t> wfbuf;
	{
		auto r = reader(hdr.WaveFormatExSize, true);
		wfbuf.insert(wfbuf.end(), r.begin(), r.end());
	}
	auto& wfex = *reinterpret_cast<WAVEFORMATEX*>(&wfbuf[0]);

	auto pos = sizeof hdr + hdr.WaveFormatExSize;
	while (pos < hdr.RemainingSize + 8) {
		struct CodeAndLen {
			LE<uint32_t> Code;
			LE<uint32_t> Len;
		};
		const auto sectionHdr = *reinterpret_cast<const CodeAndLen*>(reader(sizeof CodeAndLen, true).data());
		pos += sizeof sectionHdr;
		if (sectionHdr.Code == 0x61746164U) {  // "data"
			auto r = reader(sectionHdr.Len, true);

			auto res = SoundEntry{
				.Header = {
					.StreamSize = sectionHdr.Len,
					.ChannelCount = wfex.nChannels,
					.SamplingRate = wfex.nSamplesPerSec,
					.Unknown_0x02E = 0,
				},
				.Data = {r.begin(), r.end()},
			};

			switch (wfex.wFormatTag) {
				case WAVE_FORMAT_PCM:
					res.Header.Format = SoundEntryHeader::EntryFormat_WaveFormatPcm;
					break;
				case WAVE_FORMAT_ADPCM:
					res.Header.Format = SoundEntryHeader::EntryFormat_WaveFormatAdpcm;
					res.ExtraData = std::move(wfbuf);
					break;
				default:
					throw std::invalid_argument("wave format not supported");
			}

			return res;
		}
		pos += sectionHdr.Len;
	}
	throw std::invalid_argument("No data section found");
}

Sqex::Sound::ScdWriter::SoundEntry Sqex::Sound::ScdWriter::SoundEntry::FromOgg(const std::function<std::span<uint8_t>(size_t len, bool throwOnIncompleteRead)>& reader) {
	ogg_sync_state oy{};
	ogg_sync_init(&oy);
	const auto oyCleanup = Utils::CallOnDestruction([&oy] { ogg_sync_clear(&oy); });
	
	vorbis_info vi{};
	vorbis_info_init(&vi);
	const auto viCleanup = Utils::CallOnDestruction([&vi] { vorbis_info_clear(&vi); });

	vorbis_comment vc{};
	vorbis_comment_init(&vc);
	const auto vcCleanup = Utils::CallOnDestruction([&vc] { vorbis_comment_clear(&vc); });
	
	ogg_stream_state os{};
	Utils::CallOnDestruction osCleanup;
	
	std::vector<uint8_t> header;
	std::vector<uint8_t> data;
	std::vector<uint32_t> seekTable;
	std::vector<uint32_t> seekTableSamples;
	uint32_t loopStartSample = 0, loopEndSample = 0;
	uint32_t loopStartBytes = 0, loopEndBytes = 0;
	ogg_page og{};
	ogg_packet op{};
	for (size_t packetIndex = 0, pageIndex = 0; ; ) {
		const auto read = reader(4096, false);
		if (read.empty())
			break;
		if (const auto buffer = ogg_sync_buffer(&oy, static_cast<uint32_t>(read.size())))
			memcpy(buffer, read.data(), read.size());
		else
			throw std::runtime_error("ogg_sync_buffer failed");
		if (0 != ogg_sync_wrote(&oy, static_cast<uint32_t>(read.size())))
			throw std::runtime_error("ogg_sync_wrote failed");

		for (;; ++pageIndex) {
			if (auto r = ogg_sync_pageout(&oy, &og); r == -1)
				throw std::invalid_argument("ogg_sync_pageout failed");
			else if (r == 0)
				break;

			if (pageIndex == 0) {
				if (0 != ogg_stream_init(&os, ogg_page_serialno(&og)))
					throw std::runtime_error("ogg_stream_init failed");
				osCleanup = [&os] { ogg_stream_clear(&os); };
			}

			if (0 != ogg_stream_pagein(&os, &og))
				throw std::runtime_error("ogg_stream_pagein failed");
			
			if (packetIndex < 3) {
				header.insert(header.end(), og.header, og.header + og.header_len);
				header.insert(header.end(), og.body, og.body + og.body_len);
			} else {
				const auto sampleIndex = ogg_page_granulepos(&og);
				if (loopStartSample && loopStartBytes == 0){
					if (loopStartSample < sampleIndex)
						loopStartBytes = seekTable.back();
					else if (loopStartSample == sampleIndex)
						loopStartBytes = static_cast<uint32_t>(data.size());
				}
					
				seekTable.push_back(static_cast<uint32_t>(data.size()));
				seekTableSamples.push_back(static_cast<uint32_t>(sampleIndex));
				data.insert(data.end(), og.header, og.header + og.header_len);
				data.insert(data.end(), og.body, og.body + og.body_len);

				if (loopEndSample && loopEndBytes == 0 && loopEndSample < sampleIndex)
					loopEndBytes = static_cast<uint32_t>(data.size());
			}
			
			for (;; ++packetIndex) {
				if (auto r = ogg_stream_packetout(&os, &op); r == -1)
					throw std::runtime_error("ogg_stream_packetout failed");
				else if (r == 0)
					break;
		
				if (packetIndex < 3) {
					if (const auto res = vorbis_synthesis_headerin(&vi, &vc, &op))
						throw std::runtime_error(std::format("vorbis_synthesis_headerin failed: {}", res));
					if (packetIndex == 2) {
						char **comments=vc.user_comments;
						while (*comments) {
							if (_strnicmp(*comments, "LoopStart=", 10) == 0)
								loopStartSample = std::strtoul(*comments + 10, nullptr, 10);
							else if (_strnicmp(*comments, "LoopEnd=", 8) == 0)
								loopEndSample = std::strtoul(*comments + 8, nullptr, 10);
							++comments;
						}
					}
				}
			}

			if (ogg_page_eos(&og)) {
				const auto sampleIndex = ogg_page_granulepos(&og);
				if (loopEndSample && !loopEndBytes)
					loopEndBytes = static_cast<uint32_t>(data.size());
				std::vector<uint8_t> oggHeaderBytes;
				oggHeaderBytes.reserve(sizeof SoundEntryOggHeader + std::span(seekTable).size_bytes() + header.size());
				oggHeaderBytes.resize(sizeof SoundEntryOggHeader);
				const auto seekTableSpan = std::span(reinterpret_cast<const uint8_t*>(&seekTable[0]), seekTable.size() * sizeof seekTable[0]);
				oggHeaderBytes.insert(oggHeaderBytes.end(), seekTableSpan.begin(), seekTableSpan.end());
				oggHeaderBytes.insert(oggHeaderBytes.end(), header.begin(), header.end());
				auto& oggHeader = *reinterpret_cast<SoundEntryOggHeader*>(&oggHeaderBytes[0]);
				oggHeader.Version = 0x02;
				oggHeader.HeaderSize = 0x20;
				oggHeader.SeekTableSize = static_cast<uint32_t>(seekTableSpan.size_bytes());
				oggHeader.VorbisHeaderSize = static_cast<uint32_t>(header.size());
				return Sqex::Sound::ScdWriter::SoundEntry{
					.Header = {
						.StreamSize = static_cast<uint32_t>(data.size()),
						.ChannelCount = static_cast<uint32_t>(vi.channels),
						.SamplingRate = static_cast<uint32_t>(vi.rate),
						.Format = SoundEntryHeader::EntryFormat::EntryFormat_Ogg,
						.LoopStartOffset = loopStartBytes,
						.LoopEndOffset = loopEndBytes,
						.StreamOffset = static_cast<uint32_t>(oggHeaderBytes.size()),
						.Unknown_0x02E = 0,
					},
					.ExtraData = std::move(oggHeaderBytes),
					.Data = std::move(data),
				};
			}
		}
	}
	
	throw std::invalid_argument("ogg: eos not found");
}

Sqex::Sound::ScdWriter::SoundEntry Sqex::Sound::ScdWriter::SoundEntry::EmptyEntry() {
	return {
		.Header = {
			.Format = SoundEntryHeader::EntryFormat_Empty,
		},
	};
}

size_t Sqex::Sound::ScdWriter::SoundEntry::CalculateEntrySize() const {
	size_t auxLength = 0;
	for (const auto& aux : AuxChunks | std::views::values)
		auxLength += 8 + aux.size();

	return sizeof SoundEntryHeader + auxLength + ExtraData.size() + Data.size();
}

void Sqex::Sound::ScdWriter::SoundEntry::ExportTo(std::vector<uint8_t>& res) const {
	const auto insert = [&res](const auto& v) {
		res.insert(res.end(), reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + sizeof v);
	};

	const auto entrySize = CalculateEntrySize();
	auto hdr = Header;
	hdr.StreamOffset = static_cast<uint32_t>(entrySize - Data.size() - sizeof hdr);
	hdr.StreamSize = static_cast<uint32_t>(Data.size());
	hdr.AuxChunkCount = static_cast<uint32_t>(AuxChunks.size());

	res.reserve(res.size() + entrySize);
	insert(hdr);
	for (const auto& [name, aux] : AuxChunks) {
		if (name.size() != 4)
			throw std::invalid_argument("Length of name must be 4");
		res.insert(res.end(), name.begin(), name.end());
		insert(static_cast<uint32_t>(8 + aux.size()));
		res.insert(res.end(), aux.begin(), aux.end());
	}
	res.insert(res.end(), ExtraData.begin(), ExtraData.end());
	res.insert(res.end(), Data.begin(), Data.end());
}

void Sqex::Sound::ScdWriter::SetTable1(std::vector<std::vector<uint8_t>> t) {
	m_table1 = std::move(t);
}

void Sqex::Sound::ScdWriter::SetTable2(std::vector<std::vector<uint8_t>> t) {
	m_table2 = std::move(t);
}

void Sqex::Sound::ScdWriter::SetTable4(std::vector<std::vector<uint8_t>> t) {
	m_table4 = std::move(t);
}

void Sqex::Sound::ScdWriter::SetTable5(std::vector<std::vector<uint8_t>> t) {
	// Apparently the game still plays sounds without this table

	m_table5 = std::move(t);
}

void Sqex::Sound::ScdWriter::SetSoundEntry(size_t index, SoundEntry entry) {
	if (m_soundEntries.size() <= index)
		m_soundEntries.resize(index + 1);
	m_soundEntries[index] = std::move(entry);
}

std::vector<uint8_t> Sqex::Sound::ScdWriter::Export() const {
	if (m_table1.size() != m_table4.size())
		throw std::invalid_argument("table1.size != table4.size");

	const auto table1OffsetsOffset = sizeof ScdHeader + sizeof Offsets;
	const auto table2OffsetsOffset = Sqex::Align<size_t>(table1OffsetsOffset + sizeof uint32_t * (1 + m_table1.size()), 0x10).Alloc;
	const auto soundEntryOffsetsOffset = Sqex::Align<size_t>(table2OffsetsOffset + sizeof uint32_t * (1 + m_table2.size()), 0x10).Alloc;
	const auto table4OffsetsOffset = Sqex::Align<size_t>(soundEntryOffsetsOffset + sizeof uint32_t * (1 + m_soundEntries.size()), 0x10).Alloc;
	const auto table5OffsetsOffset = Sqex::Align<size_t>(table4OffsetsOffset + sizeof uint32_t * (1 + m_table4.size()), 0x10).Alloc;

	std::vector<uint8_t> res;
	size_t requiredSize = table5OffsetsOffset + sizeof uint32_t * 4;
	for (const auto& item : m_table4)
		requiredSize += item.size();
	for (const auto& item : m_table1)
		requiredSize += item.size();
	for (const auto& item : m_table2)
		requiredSize += item.size();
	for (const auto& item : m_table5)
		requiredSize += item.size();
	for (const auto& item : m_soundEntries)
		requiredSize += item.CalculateEntrySize();
	requiredSize = Sqex::Align<size_t>(requiredSize, 0x10).Alloc;
	res.reserve(requiredSize);

	res.resize(table5OffsetsOffset + sizeof uint32_t * 4);

	for (size_t i = 0; i < m_table4.size(); ++i) {
		reinterpret_cast<uint32_t*>(&res[table4OffsetsOffset])[i] = static_cast<uint32_t>(res.size());
		res.insert(res.end(), m_table4[i].begin(), m_table4[i].end());
	}
	for (size_t i = 0; i < m_table1.size(); ++i) {
		reinterpret_cast<uint32_t*>(&res[table1OffsetsOffset])[i] = static_cast<uint32_t>(res.size());
		res.insert(res.end(), m_table1[i].begin(), m_table1[i].end());
	}
	for (size_t i = 0; i < m_table2.size(); ++i) {
		reinterpret_cast<uint32_t*>(&res[table2OffsetsOffset])[i] = static_cast<uint32_t>(res.size());
		res.insert(res.end(), m_table2[i].begin(), m_table2[i].end());
	}
	for (size_t i = 0; i < m_table5.size() && i < 3; ++i) {
		if (m_table5[i].empty())
			break;
		reinterpret_cast<uint32_t*>(&res[table5OffsetsOffset])[i] = static_cast<uint32_t>(res.size());
		res.insert(res.end(), m_table5[i].begin(), m_table5[i].end());
	}
	for (size_t i = 0; i < m_soundEntries.size(); ++i) {
		reinterpret_cast<uint32_t*>(&res[soundEntryOffsetsOffset])[i] = static_cast<uint32_t>(res.size());
		m_soundEntries[i].ExportTo(res);
	}

	*reinterpret_cast<ScdHeader*>(&res[0]) = {
		.SedbVersion = ScdHeader::SedbVersion_FFXIV,
		.EndianFlag = ScdHeaderEndiannessFlag::LittleEndian,
		.SscfVersion = ScdHeader::SscfVersion_FFXIV,
		.HeaderSize = sizeof ScdHeader,
		.FileSize = static_cast<uint32_t>(requiredSize),
	};
	memcpy(reinterpret_cast<ScdHeader*>(&res[0])->SedbSignature,
		ScdHeader::SedbSignature_Value,
		sizeof ScdHeader::SedbSignature_Value);
	memcpy(reinterpret_cast<ScdHeader*>(&res[0])->SscfSignature,
		ScdHeader::SscfSignature_Value,
		sizeof ScdHeader::SscfSignature_Value);

	*reinterpret_cast<Offsets*>(&res[sizeof ScdHeader]) = {
		.Table1And4EntryCount = static_cast<uint16_t>(m_table1.size()),
		.Table2EntryCount = static_cast<uint16_t>(m_table2.size()),
		.SoundEntryCount = static_cast<uint16_t>(m_soundEntries.size()),
		.Unknown_0x006 = 0,  // ?
		.Table2Offset = static_cast<uint32_t>(table2OffsetsOffset),
		.SoundEntryOffset = static_cast<uint32_t>(soundEntryOffsetsOffset),
		.Table4Offset = static_cast<uint32_t>(table4OffsetsOffset),
		.Table5Offset = static_cast<uint32_t>(table5OffsetsOffset),
		.Unknown_0x01C = 0,  // ?
	};

	res.resize(requiredSize);

	return res;
}
