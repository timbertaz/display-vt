// Small NMEA 0183 parser for a u-blox NEO-6M (GY-GPS6MV2) at its default settings.
// Platform independent so it can be tested on a PC.
//
// Uses:  RMC (speed over ground in knots, fix valid A/V)
//        VTG (speed over ground in km/h)
//        GGA (number of satellites in use)
// Works with any talker ID ($GP..., $GN..., $GL...).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace nmea
{
	struct Fix
	{
		bool valid = false;        // receiver says the position/speed is usable
		float speedKmh = 0.0f;     // speed over ground
		int satellites = 0;        // satellites used in the fix
		std::uint32_t sentences = 0; // good sentences seen (to tell "no GPS" from "no fix")
	};

	class Parser
	{
	public:
		// Feed raw bytes from the UART. Returns true when a sentence updated the fix.
		bool feed(char c)
		{
			if ('$' == c)
			{
				line.clear();
				inSentence = true;
				line += c;
				return false;
			}
			if (!inSentence)
			{
				return false;
			}
			if (('\r' == c) || ('\n' == c))
			{
				inSentence = false;
				return parse_line(line);
			}
			if (line.size() < 100)
			{
				line += c;
			}
			else
			{
				inSentence = false; // too long: not a valid NMEA sentence
			}
			return false;
		}

		const Fix &fix() const { return current; }

		// Parse one complete sentence like "$GPRMC,...*7A". Public for testing.
		bool parse_line(const std::string &sentence)
		{
			if (!checksum_ok(sentence))
			{
				return false;
			}
			std::size_t star = sentence.find('*');
			std::vector<std::string> f = split(sentence.substr(1, star - 1));
			if (f.empty() || (f[0].size() < 5))
			{
				return false;
			}
			const std::string type = f[0].substr(f[0].size() - 3); // drop the talker ID

			if (("RMC" == type) && (f.size() > 7))
			{
				current.valid = ("A" == f[2]);
				if (current.valid && !f[7].empty() && !haveVtgSpeed)
				{
					current.speedKmh = static_cast<float>(std::atof(f[7].c_str()) * 1.852); // knots -> km/h
				}
				if (!current.valid)
				{
					current.speedKmh = 0.0f;
				}
				current.sentences++;
				return true;
			}
			if (("VTG" == type) && (f.size() > 7))
			{
				// Field 9 (NMEA 2.3+) is the mode: N = not valid
				bool modeValid = (f.size() <= 9) || (f[9].empty()) || (f[9][0] != 'N');
				if (modeValid && !f[7].empty())
				{
					current.speedKmh = static_cast<float>(std::atof(f[7].c_str()));
					haveVtgSpeed = true;
				}
				current.sentences++;
				return true;
			}
			if (("GGA" == type) && (f.size() > 7))
			{
				current.satellites = std::atoi(f[7].c_str());
				current.sentences++;
				return true;
			}
			return false;
		}

	private:
		static bool checksum_ok(const std::string &s)
		{
			std::size_t star = s.find('*');
			if ((s.size() < 4) || ('$' != s[0]) || (std::string::npos == star) || (star + 3 > s.size()))
			{
				return false;
			}
			std::uint8_t sum = 0;
			for (std::size_t i = 1; i < star; i++)
			{
				sum ^= static_cast<std::uint8_t>(s[i]);
			}
			unsigned long given = std::strtoul(s.substr(star + 1, 2).c_str(), nullptr, 16);
			return given == sum;
		}

		static std::vector<std::string> split(const std::string &s)
		{
			std::vector<std::string> out;
			std::string field;
			for (char c : s)
			{
				if (',' == c)
				{
					out.push_back(field);
					field.clear();
				}
				else
				{
					field += c;
				}
			}
			out.push_back(field);
			return out;
		}

		std::string line;
		bool inSentence = false;
		bool haveVtgSpeed = false; // prefer VTG's km/h once we have seen it
		Fix current;
	};
}
