// MJPEG-over-HTTP stream parser (the camera node's http://192.168.4.1:81/stream).
//
// Fed with whatever bytes arrive from the socket; calls onFrame for every complete JPEG.
// Handles the HTTP response header, chunked transfer encoding (what ESP-IDF's web server
// sends) and the multipart parts with a Content-Length header.
// No ESP-IDF dependencies, so it is tested on the PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace mjpeg
{
	class StreamParser
	{
	public:
		using FrameHandler = std::function<void(const std::uint8_t *jpeg, std::size_t length)>;

		// buffer: where a frame is collected (frames bigger than capacity are skipped)
		StreamParser(std::uint8_t *buffer, std::size_t capacity, FrameHandler onFrame) :
		  frame(buffer), frameCapacity(capacity), handler(std::move(onFrame))
		{
		}

		// Returns false when the stream can't be used (not HTTP 200)
		bool feed(const std::uint8_t *data, std::size_t length)
		{
			for (std::size_t i = 0; (i < length) && !failed; i++)
			{
				http_byte(data[i]);
			}
			return !failed;
		}

		bool ok() const { return !failed; }

		// Collect the next frame somewhere else (call from inside the frame handler)
		void set_buffer(std::uint8_t *buffer) { frame = buffer; }
		std::uint32_t skipped_frames() const { return skipped; }

	private:
		enum class Http
		{
			StatusLine,
			Headers,
			ChunkSize,
			ChunkData,
			ChunkEnd,
			Body
		};
		enum class Part
		{
			Headers,
			Data
		};

		// ---- line collector shared by all text sections ----
		bool line_byte(std::uint8_t c)
		{
			if ('\n' == c)
			{
				line[lineLength] = '\0';
				if ((lineLength > 0) && ('\r' == line[lineLength - 1]))
				{
					line[lineLength - 1] = '\0';
				}
				lineLength = 0;
				return true; // a complete line is in `line`
			}
			if (lineLength < sizeof(line) - 1)
			{
				line[lineLength++] = static_cast<char>(c);
			}
			return false;
		}

		static bool starts_with_nocase(const char *text, const char *prefix)
		{
			for (; '\0' != *prefix; text++, prefix++)
			{
				char a = *text;
				char b = *prefix;
				if ((a >= 'A') && (a <= 'Z'))
				{
					a = static_cast<char>(a - 'A' + 'a');
				}
				if ((b >= 'A') && (b <= 'Z'))
				{
					b = static_cast<char>(b - 'A' + 'a');
				}
				if (a != b)
				{
					return false;
				}
			}
			return true;
		}

		// ---- HTTP layer ----
		void http_byte(std::uint8_t c)
		{
			switch (http)
			{
				case Http::StatusLine:
					if (line_byte(c))
					{
						// "HTTP/1.1 200 OK"
						const char *space = std::strchr(line, ' ');
						if ((nullptr == space) || (200 != std::atoi(space + 1)))
						{
							failed = true;
						}
						http = Http::Headers;
					}
					break;

				case Http::Headers:
					if (line_byte(c))
					{
						if ('\0' == line[0])
						{
							http = chunked ? Http::ChunkSize : Http::Body;
						}
						else if (starts_with_nocase(line, "transfer-encoding:") && (nullptr != std::strstr(line, "chunked")))
						{
							chunked = true;
						}
					}
					break;

				case Http::ChunkSize:
					if (line_byte(c))
					{
						chunkRemaining = std::strtoul(line, nullptr, 16);
						http = (0 == chunkRemaining) ? Http::ChunkEnd : Http::ChunkData;
						if (0 == chunkRemaining)
						{
							failed = true; // last chunk: the camera ended the stream
						}
					}
					break;

				case Http::ChunkData:
					part_byte(c);
					if (0 == --chunkRemaining)
					{
						http = Http::ChunkEnd;
					}
					break;

				case Http::ChunkEnd:
					// the "\r\n" after each chunk
					if ('\n' == c)
					{
						http = Http::ChunkSize;
					}
					break;

				case Http::Body:
					part_byte(c);
					break;
			}
		}

		// ---- multipart layer ----
		void part_byte(std::uint8_t c)
		{
			if (Part::Headers == part)
			{
				if (line_byte(c))
				{
					if (starts_with_nocase(line, "content-length:"))
					{
						partLength = std::strtoul(line + 15, nullptr, 10);
					}
					else if (('\0' == line[0]) && (partLength > 0))
					{
						part = Part::Data;
						received = 0;
					}
				}
				return;
			}

			if (partLength <= frameCapacity)
			{
				frame[received] = c;
			}
			if (++received == partLength)
			{
				if (partLength <= frameCapacity)
				{
					handler(frame, partLength);
				}
				else
				{
					skipped++;
				}
				partLength = 0;
				part = Part::Headers;
			}
		}

		std::uint8_t *frame;
		std::size_t frameCapacity;
		FrameHandler handler;

		Http http = Http::StatusLine;
		Part part = Part::Headers;
		bool chunked = false;
		bool failed = false;
		char line[256] = {};
		std::size_t lineLength = 0;
		std::size_t chunkRemaining = 0;
		std::size_t partLength = 0;
		std::size_t received = 0;
		std::uint32_t skipped = 0;
	};
}
