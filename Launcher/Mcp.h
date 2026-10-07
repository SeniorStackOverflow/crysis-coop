// Crysis Coop: "CrysisCoop.exe -coop_mcp" is an MCP server (Model Context
// Protocol, JSON-RPC over stdio) through which an AI agent plays the AI
// companion (see Code/Mod/CoopAgent.h): it sees what the companion sees and
// gives it orders. It talks to the companion's game at 127.0.0.1:47810
// (-coop_mcp_port <port> for another one).
#pragma once

#include <string>

namespace Mcp
{
	int Run(const std::wstring& args);
}
