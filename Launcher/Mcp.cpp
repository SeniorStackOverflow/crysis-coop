// Crysis Coop: the MCP server for AI agents (see Mcp.h).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "Mcp.h"
#include "project.h"

namespace
{
	// ------------------------------------------------------------------
	// JSON: just what the protocol needs
	struct JValue
	{
		enum EType { eNull, eBool, eNumber, eString, eArray, eObject } type = eNull;
		bool b = false;
		double n = 0;
		std::string s;
		std::vector<JValue> a;
		std::vector<std::pair<std::string, JValue>> o;

		const JValue* Get(const char* key) const
		{
			if (type != eObject)
				return nullptr;
			for (const auto& kv : o)
				if (kv.first == key)
					return &kv.second;
			return nullptr;
		}
		std::string Str(const char* key, const char* def = "") const
		{
			const JValue* v = Get(key);
			if (!v)
				return def;
			if (v->type == eString)
				return v->s;
			if (v->type == eNumber)
			{
				char buf[64];
				sprintf_s(buf, "%g", v->n);
				return buf;
			}
			if (v->type == eBool)
				return v->b ? "true" : "false";
			return def;
		}
	};

	struct JParser
	{
		const char* p;
		const char* end;

		void Space()
		{
			while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
				++p;
		}

		static void Utf8(std::string& out, unsigned int c)
		{
			if (c < 0x80)
				out += (char)c;
			else if (c < 0x800)
			{
				out += (char)(0xC0 | (c >> 6));
				out += (char)(0x80 | (c & 0x3F));
			}
			else if (c < 0x10000)
			{
				out += (char)(0xE0 | (c >> 12));
				out += (char)(0x80 | ((c >> 6) & 0x3F));
				out += (char)(0x80 | (c & 0x3F));
			}
			else
			{
				out += (char)(0xF0 | (c >> 18));
				out += (char)(0x80 | ((c >> 12) & 0x3F));
				out += (char)(0x80 | ((c >> 6) & 0x3F));
				out += (char)(0x80 | (c & 0x3F));
			}
		}

		bool Hex4(unsigned int& c)
		{
			if (end - p < 4)
				return false;
			c = 0;
			for (int i = 0; i < 4; ++i)
			{
				const char h = *p++;
				c <<= 4;
				if (h >= '0' && h <= '9') c |= h - '0';
				else if (h >= 'a' && h <= 'f') c |= h - 'a' + 10;
				else if (h >= 'A' && h <= 'F') c |= h - 'A' + 10;
				else return false;
			}
			return true;
		}

		bool String(std::string& out)
		{
			if (p >= end || *p != '"')
				return false;
			++p;
			while (p < end && *p != '"')
			{
				if (*p != '\\')
				{
					out += *p++;
					continue;
				}
				if (++p >= end)
					return false;
				const char e = *p++;
				switch (e)
				{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					unsigned int c;
					if (!Hex4(c))
						return false;
					if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
					{
						p += 2;
						unsigned int low;
						if (!Hex4(low))
							return false;
						c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
					}
					Utf8(out, c);
					break;
				}
				default: return false;
				}
			}
			if (p >= end)
				return false;
			++p;
			return true;
		}

		bool Value(JValue& v, int depth = 0)
		{
			if (depth > 32)
				return false;
			Space();
			if (p >= end)
				return false;
			if (*p == '{')
			{
				v.type = JValue::eObject;
				++p;
				Space();
				if (p < end && *p == '}')
				{
					++p;
					return true;
				}
				for (;;)
				{
					Space();
					std::pair<std::string, JValue> kv;
					if (!String(kv.first))
						return false;
					Space();
					if (p >= end || *p++ != ':')
						return false;
					if (!Value(kv.second, depth + 1))
						return false;
					v.o.push_back(std::move(kv));
					Space();
					if (p < end && *p == ',')
					{
						++p;
						continue;
					}
					if (p < end && *p == '}')
					{
						++p;
						return true;
					}
					return false;
				}
			}
			if (*p == '[')
			{
				v.type = JValue::eArray;
				++p;
				Space();
				if (p < end && *p == ']')
				{
					++p;
					return true;
				}
				for (;;)
				{
					JValue item;
					if (!Value(item, depth + 1))
						return false;
					v.a.push_back(std::move(item));
					Space();
					if (p < end && *p == ',')
					{
						++p;
						continue;
					}
					if (p < end && *p == ']')
					{
						++p;
						return true;
					}
					return false;
				}
			}
			if (*p == '"')
			{
				v.type = JValue::eString;
				return String(v.s);
			}
			if (end - p >= 4 && !strncmp(p, "true", 4)) { v.type = JValue::eBool; v.b = true; p += 4; return true; }
			if (end - p >= 5 && !strncmp(p, "false", 5)) { v.type = JValue::eBool; v.b = false; p += 5; return true; }
			if (end - p >= 4 && !strncmp(p, "null", 4)) { v.type = JValue::eNull; p += 4; return true; }
			char* after = nullptr;
			std::string num;
			while (p < end && (strchr("+-0123456789.eE", *p) && *p))
				num += *p++;
			if (num.empty())
				return false;
			v.type = JValue::eNumber;
			v.n = strtod(num.c_str(), &after);
			return true;
		}
	};

	bool Parse(const std::string& text, JValue& v)
	{
		JParser parser = { text.data(), text.data() + text.size() };
		return parser.Value(v);
	}

	std::string Esc(const std::string& text)
	{
		std::string out = "\"";
		for (unsigned char c : text)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					sprintf_s(buf, "\\u%04x", c);
					out += buf;
				}
				else
					out += (char)c;
			}
		}
		return out + "\"";
	}

	std::string Write(const JValue& v)
	{
		switch (v.type)
		{
		case JValue::eBool: return v.b ? "true" : "false";
		case JValue::eNumber:
		{
			char buf[64];
			sprintf_s(buf, "%.15g", v.n);
			return buf;
		}
		case JValue::eString: return Esc(v.s);
		case JValue::eArray:
		{
			std::string out = "[";
			for (size_t i = 0; i < v.a.size(); ++i)
				out += (i ? "," : "") + Write(v.a[i]);
			return out + "]";
		}
		case JValue::eObject:
		{
			std::string out = "{";
			for (size_t i = 0; i < v.o.size(); ++i)
				out += (i ? "," : "") + Esc(v.o[i].first) + ":" + Write(v.o[i].second);
			return out + "}";
		}
		default: return "null";
		}
	}

	// ------------------------------------------------------------------
	// stdio: a JSON-RPC message per line
	HANDLE s_stdin = INVALID_HANDLE_VALUE, s_stdout = INVALID_HANDLE_VALUE;
	std::string s_inBuf;

	bool ReadLine(std::string& line)
	{
		for (;;)
		{
			const size_t nl = s_inBuf.find('\n');
			if (nl != std::string::npos)
			{
				line = s_inBuf.substr(0, nl);
				s_inBuf.erase(0, nl + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				return true;
			}
			char buf[8192];
			DWORD got = 0;
			if (!ReadFile(s_stdin, buf, sizeof(buf), &got, nullptr) || got == 0)
				return false;
			s_inBuf.append(buf, got);
		}
	}

	void Send(const std::string& json)
	{
		const std::string line = json + "\n";
		DWORD written = 0;
		WriteFile(s_stdout, line.data(), (DWORD)line.size(), &written, nullptr);
	}

	// ------------------------------------------------------------------
	// the companion's game
	int s_port = 0;        // -coop_mcp_port; else the port the companion's game wrote (agent.port)

	// the port of the companion's game: "<port> <process>" in agent.port (the
	// usual port may be one the system keeps from programs); 0: it is not running
	int CompanionPort()
	{
		if (s_port > 0)
			return s_port;
		wchar_t local[MAX_PATH] = L"";
		GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		int port = 0;
		unsigned pid = 0;
		if (FILE* f = _wfopen((std::wstring(local) + L"\\CrysisCoop\\companion\\agent.port").c_str(), L"rb"))
		{
			if (fscanf(f, "%d %u", &port, &pid) < 1)
				port = 0;
			fclose(f);
		}
		if (port <= 0 || port > 65535)
			return 47810;
		if (pid)
		{
			// a file left by a game that is gone: nothing listens (and the port may be someone else's)
			HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (!h)
				return 0;
			const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
			CloseHandle(h);
			if (!alive)
				return 0;
		}
		return port;
	}
	SOCKET s_sock = INVALID_SOCKET;
	std::string s_sockBuf;
	int s_nextId = 1;

	void Disconnect()
	{
		if (s_sock != INVALID_SOCKET)
			closesocket(s_sock);
		s_sock = INVALID_SOCKET;
		s_sockBuf.clear();
	}

	bool Connect()
	{
		if (s_sock != INVALID_SOCKET)
			return true;
		const int port = CompanionPort();
		if (port <= 0)
			return false;
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			return false;
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
		{
			closesocket(s);
			return false;
		}
		DWORD timeout = 15000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
		s_sock = s;
		return true;
	}

	// a request to the companion; its JSON reply ({"ok":...}) or an error text
	bool Ask(const std::string& command, JValue& reply, std::string& error)
	{
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			if (!Connect())
			{
				error = "The AI companion is not running. In the game: host a co-op game, then Esc > Co-op game > turn on \"AI companion\"; "
					"it joins in about a minute (its game starts in the background).";
				return false;
			}
			const int id = s_nextId++;
			const std::string line = std::to_string(id) + " " + command + "\n";
			if (send(s_sock, line.data(), (int)line.size(), 0) != (int)line.size())
			{
				Disconnect();
				continue;
			}
			const std::string prefix = std::to_string(id) + " ";
			for (;;)
			{
				const size_t nl = s_sockBuf.find('\n');
				if (nl != std::string::npos)
				{
					std::string got = s_sockBuf.substr(0, nl);
					s_sockBuf.erase(0, nl + 1);
					if (got.compare(0, prefix.size(), prefix) != 0)
						continue;       // an answer to an older question
					if (!Parse(got.substr(prefix.size()), reply))
					{
						error = "the companion's answer could not be read";
						return false;
					}
					return true;
				}
				char buf[65536];
				const int n = recv(s_sock, buf, sizeof(buf), 0);
				if (n <= 0)
				{
					Disconnect();
					break;
				}
				s_sockBuf.append(buf, n);
			}
		}
		error = "the AI companion's game did not answer (is it loading?)";
		return false;
	}

	// ------------------------------------------------------------------
	// the tools
	struct STool
	{
		const char* name;
		const char* description;
		const char* schema;     // the input schema's properties and required list
	};

	const STool s_tools[] = {
		{ "observe", "Look at the game as the AI companion (the second player). Returns JSON: 'me' (health, suit, weapon and ammo, position, facing, vehicle), "
			"'order' (the current order and what the companion is doing now), 'teammates' (the human host and others: health, down, distance, compass direction, "
			"bearing in degrees from where the companion looks: 0 ahead, 90 to the right, -90 to the left), 'enemies' (nearest 10: name, distance, direction, bearing, "
			"visible, alerted 0-2, vehicle), 'vehicles' nearby, 'events' and 'chat' since the last look. Call it often: every few seconds of play.",
			"{\"type\":\"object\",\"properties\":{}}" },
		{ "play_freely", "Let the companion play on its own (the default): follow the host, fire at enemies it sees, revive a downed teammate, ride along in the host's vehicle.",
			"{\"type\":\"object\",\"properties\":{}}" },
		{ "follow", "Follow a player (the host when no name is given), fighting on the way.",
			"{\"type\":\"object\",\"properties\":{\"player\":{\"type\":\"string\",\"description\":\"player name; empty = the host\"}}}" },
		{ "hold", "Stay where the companion is now (it still fires at enemies unless fire_at_will is off).",
			"{\"type\":\"object\",\"properties\":{}}" },
		{ "go_to", "Go to a place, then hold there: a player's or an object's name, or world coordinates x y z (as in observe's positions).",
			"{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"a player or object name\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}" },
		{ "move", "Walk some meters in a direction, then hold: forward/back/left/right (from where the companion looks) or north/south/east/west.",
			"{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"forward\",\"back\",\"left\",\"right\",\"north\",\"south\",\"east\",\"west\"]},\"meters\":{\"type\":\"number\"}},\"required\":[\"direction\",\"meters\"]}" },
		{ "attack", "Attack an enemy by name (from observe), or the nearest one. The companion goes after it until it is dead, then plays freely again.",
			"{\"type\":\"object\",\"properties\":{\"enemy\":{\"type\":\"string\",\"description\":\"the enemy's name; empty = the nearest\"}}}" },
		{ "revive", "Run to a downed teammate and revive them (it does this by itself too, unless told to hold or go somewhere).",
			"{\"type\":\"object\",\"properties\":{\"player\":{\"type\":\"string\",\"description\":\"empty = the nearest downed teammate\"}}}" },
		{ "fire_at_will", "Whether the companion shoots at enemies it sees on its own (true, the default) or holds fire (false: stealth) until told to attack.",
			"{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"enabled\"]}" },
		{ "say", "Say something in the game's chat: the human host reads it on the screen. Keep it short.",
			"{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}" },
		{ "use", "Press the use key on what is in front of the companion (a door, a switch, a pick-up).",
			"{\"type\":\"object\",\"properties\":{}}" },
		{ "switch_weapon", "Take the next weapon, or one by its class name (SCAR, FY71, SMG, Shotgun, SOCOM, DSG1, LAW...).",
			"{\"type\":\"object\",\"properties\":{\"weapon\":{\"type\":\"string\",\"description\":\"empty = the next one\"}}}" },
		{ "suit_mode", "Set the nanosuit mode.",
			"{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"armor\",\"speed\",\"strength\",\"cloak\"]}},\"required\":[\"mode\"]}" },
		{ "screenshot", "What the companion sees now, as a small picture. Its game usually runs without graphics to stay light: then there is no picture, and observe is the way to look.",
			"{\"type\":\"object\",\"properties\":{}}" },
	};

	const char* INSTRUCTIONS =
		"You are the second player of a Crysis (2007) co-op campaign: the AI companion, in the same squad as the human who hosts the game. "
		"The companion's body is played by a bot in its game: by default it follows the host, fires at enemies it sees, revives a downed teammate and rides in "
		"the host's vehicle. You are its mind: call observe every few seconds, read the chat (the human may ask you for something), give orders when they help "
		"(attack a dangerous enemy, hold a position, go somewhere, hold fire to stay unseen), and answer the human with say, briefly. "
		"When everybody is down the team gets up at the last checkpoint. A player who is down waits for a teammate to revive them.";

	std::string Text(const std::string& text, bool error = false)
	{
		return "{\"content\":[{\"type\":\"text\",\"text\":" + Esc(text) + "}],\"isError\":" + (error ? "true" : "false") + "}";
	}

	std::string CallTool(const std::string& name, const JValue* args)
	{
		static const JValue none;
		if (!args)
			args = &none;
		std::string command;
		if (name == "observe") command = "observe";
		else if (name == "play_freely") command = "free";
		else if (name == "follow") command = "follow " + args->Str("player");
		else if (name == "hold") command = "hold";
		else if (name == "go_to")
		{
			const std::string target = args->Str("target");
			if (!target.empty())
				command = "goto " + target;
			else if (args->Get("x") && args->Get("y"))
				command = "goto " + args->Str("x") + " " + args->Str("y") + " " + args->Str("z", "0");
			else
				return Text("go_to needs a target name or x, y (and z)", true);
		}
		else if (name == "move") command = "move " + args->Str("direction", "forward") + " " + args->Str("meters", "10");
		else if (name == "attack") command = "attack " + args->Str("enemy", "nearest");
		else if (name == "revive") command = "revive " + args->Str("player");
		else if (name == "fire_at_will") command = std::string("fire ") + (args->Str("enabled", "true") == "false" ? "off" : "on");
		else if (name == "say") command = "say " + args->Str("text");
		else if (name == "use") command = "use";
		else if (name == "switch_weapon") command = "weapon " + args->Str("weapon", "next");
		else if (name == "suit_mode") command = "suit " + args->Str("mode", "armor");
		else if (name == "screenshot") command = "screenshot";
		else
			return Text("unknown tool " + name, true);
		// one line to the game
		for (char& c : command)
			if (c == '\n' || c == '\r')
				c = ' ';
		JValue reply;
		std::string error;
		if (!Ask(command, reply, error))
			return Text(error, true);
		const JValue* ok = reply.Get("ok");
		if (!ok || !ok->b)
			return Text(reply.Str("error", "the companion refused"), true);
		if (name == "screenshot")
		{
			const std::string mime = reply.Str("mime");
			if (mime != "image/jpeg" && mime != "image/png")
				return Text("the screenshot is in a format that cannot be shown (" + mime + ")", true);
			return "{\"content\":[{\"type\":\"image\",\"data\":" + Esc(reply.Str("data")) + ",\"mimeType\":" + Esc(mime) + "}],\"isError\":false}";
		}
		const JValue* result = reply.Get("result");
		if (result && result->type == JValue::eString)
			return Text(result->s);
		return Text(result ? Write(*result) : "done");
	}

	std::string ToolsList()
	{
		std::string out = "{\"tools\":[";
		for (size_t i = 0; i < sizeof(s_tools) / sizeof(s_tools[0]); ++i)
		{
			if (i)
				out += ",";
			out += "{\"name\":" + Esc(s_tools[i].name) + ",\"description\":" + Esc(s_tools[i].description) + ",\"inputSchema\":" + s_tools[i].schema + "}";
		}
		return out + "]}";
	}

	void Respond(const JValue* id, const std::string& result)
	{
		Send("{\"jsonrpc\":\"2.0\",\"id\":" + (id ? Write(*id) : std::string("null")) + ",\"result\":" + result + "}");
	}

	void RespondError(const JValue* id, int code, const std::string& message)
	{
		Send("{\"jsonrpc\":\"2.0\",\"id\":" + (id ? Write(*id) : std::string("null")) + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + Esc(message) + "}}");
	}

	void Handle(const JValue& msg)
	{
		const JValue* id = msg.Get("id");
		const std::string method = msg.Str("method");
		const JValue* params = msg.Get("params");
		if (method == "initialize")
		{
			std::string version = params ? params->Str("protocolVersion") : "";
			if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18")
				version = "2025-06-18";
			Respond(id, "{\"protocolVersion\":" + Esc(version) + ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
				"\"serverInfo\":{\"name\":\"crysis-coop\",\"title\":\"Crysis Coop AI companion\",\"version\":" + Esc(PROJECT_VERSION) + "},"
				"\"instructions\":" + Esc(INSTRUCTIONS) + "}");
		}
		else if (method == "ping")
			Respond(id, "{}");
		else if (method == "tools/list")
			Respond(id, ToolsList());
		else if (method == "tools/call")
			Respond(id, CallTool(params ? params->Str("name") : "", params ? params->Get("arguments") : nullptr));
		else if (!id || id->type == JValue::eNull)
			return;     // a notification (notifications/initialized...)
		else if (method == "resources/list")
			Respond(id, "{\"resources\":[]}");
		else if (method == "prompts/list")
			Respond(id, "{\"prompts\":[]}");
		else
			RespondError(id, -32601, "Method not found: " + method);
	}
}

int Mcp::Run(const std::wstring& args)
{
	const size_t at = args.find(L"-coop_mcp_port");
	if (at != std::wstring::npos)
		s_port = _wtoi(args.c_str() + at + 14);
	s_stdin = GetStdHandle(STD_INPUT_HANDLE);
	s_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (s_stdin == INVALID_HANDLE_VALUE || !s_stdin || s_stdout == INVALID_HANDLE_VALUE || !s_stdout)
		return 1;
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return 1;
	std::string line;
	while (ReadLine(line))
	{
		if (line.empty())
			continue;
		JValue msg;
		if (!Parse(line, msg) || msg.type != JValue::eObject)
		{
			RespondError(nullptr, -32700, "Parse error");
			continue;
		}
		Handle(msg);
	}
	Disconnect();
	WSACleanup();
	return 0;
}
