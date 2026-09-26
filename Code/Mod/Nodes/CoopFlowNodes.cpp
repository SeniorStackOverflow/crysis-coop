// Crysis Coop: flow nodes the campaign levels use to mean "the player".
//
// Game:LocalPlayer replaces CryAction's node of the same name (same ports).
// In single player and on clients it is the local player, as before. On the
// coop server it is the player closest to the graph's entity (the trigger,
// spawn point, objective... the graph belongs to), so distance checks and
// "who entered" comparisons of the level scripts work for every player and
// not only for the host.
#include "StdAfx.h"
#include "Game.h"
#include "Nodes/G2FlowBaseNode.h"
#include "CoopAI.h"
#include "CoopSave.h"
#include "Actor.h"
#include "GameRules.h"

class CFlowNode_CoopLocalPlayer : public CFlowBaseNode
{
public:
	CFlowNode_CoopLocalPlayer(SActivationInfo* pActInfo) : m_output(0), m_timer(0.0f), m_edgesChecked(false) {}

	IFlowNodePtr Clone(SActivationInfo* pActInfo)
	{
		return new CFlowNode_CoopLocalPlayer(pActInfo);
	}

	void GetConfiguration(SFlowNodeConfig& config)
	{
		static const SInputPortConfig in_ports[] =
		{
			{0}
		};
		static const SOutputPortConfig out_ports[] =
		{
			OutputPortConfig<EntityId>("entityId", _HELP("Player id (coop server: the player closest to the graph's entity)")),
			{0}
		};
		config.pInputPorts = in_ports;
		config.pOutputPorts = out_ports;
		config.sDescription = _HELP("Local player; in coop the player closest to the graph entity");
		config.SetCategory(EFLN_APPROVED);
	}

	void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo)
	{
		switch (event)
		{
		case eFE_Initialize:
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					CryLogAlways("[CoopFlow] Game:LocalPlayer: coop version active");
				}
			}
			m_output = 0;
			m_timer = 0.0f;
			pActInfo->pGraph->SetRegularlyUpdated(pActInfo->myID, true);
			break;
		case eFE_Update:
			{
				const bool coop = gEnv->bServer && CoopAI::IsCoopSession();
				if (coop && !m_edgesChecked)
				{
					m_edgesChecked = true;
					UnlinkVectorInputs(pActInfo);
				}
				if (coop)
				{
					// re-evaluated twice a second: the closest player may change
					m_timer -= gEnv->pTimer->GetFrameTime();
					if (m_output && m_timer > 0.0f)
						break;
					m_timer = 0.5f;
				}
				const EntityId id = coop ? CoopPlayer(pActInfo) : LocalPlayer();
				if (id && id != m_output)
				{
					if (m_output && coop)
					{
						IEntity* pFrom = gEnv->pEntitySystem->GetEntity(m_output);
						IEntity* pTo = gEnv->pEntitySystem->GetEntity(id);
						IEntity* pGraphEntity = gEnv->pEntitySystem->GetEntity(pActInfo->pGraph->GetGraphEntity(0));
						CoopAI::Trace("FLOWPLAYER %s: %s -> %s", pGraphEntity ? pGraphEntity->GetName() : "<no entity>",
							pFrom ? pFrom->GetName() : "-", pTo ? pTo->GetName() : "-");
					}
					m_output = id;
					ActivateOutput(pActInfo, 0, id);
				}
				// single player / client: once, like CryAction's node
				if (id && !coop)
					pActInfo->pGraph->SetRegularlyUpdated(pActInfo->myID, false);
			}
			break;
		}
	}

	virtual void GetMemoryStatistics(ICrySizer* s)
	{
		s->Add(*this);
	}

private:
	static EntityId LocalPlayer()
	{
		IActor* pActor = g_pGame->GetIGameFramework()->GetClientActor();
		return pActor ? pActor->GetEntityId() : 0;
	}

	// the living player closest to the graph's entity; the current one is kept
	// unless another is clearly closer (10 m), so the output does not flicker
	EntityId CoopPlayer(SActivationInfo* pActInfo) const
	{
		const EntityId host = LocalPlayer();
		IEntity* pGraphEntity = gEnv->pEntitySystem->GetEntity(pActInfo->pGraph->GetGraphEntity(0));
		if (!pGraphEntity)
			return m_output ? m_output : host;
		const Vec3 center = pGraphEntity->GetWorldPos();
		float current = 1e9f;
		float best = 1e9f;
		EntityId bestId = 0;
		IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor->GetHealth() <= 0 || static_cast<CActor*>(pActor)->GetSpectatorMode() != 0)
				continue;
			const float d = (pActor->GetEntity()->GetWorldPos() - center).GetLength();
			if (pActor->GetEntityId() == m_output)
				current = d;
			if (d < best)
			{
				best = d;
				bestId = pActor->GetEntityId();
			}
		}
		if (!bestId)
			return m_output ? m_output : host;
		if (m_output && current < 1e8f && best > current - 10.0f)
			return m_output;
		return bestId;
	}

	// A few level graphs also feed the player id into a vector input (Harbor:
	// Hut_Guy_on_top links it to Entity:EntityPos "pos", which then puts the
	// player at (30583,30583,30583)). In single player that happens once, at
	// the start, and the intro puts the player back; here the output is sent
	// again whenever another player becomes the closest one, so the players
	// were thrown out of the map in turns. An id is never a position: those
	// links are removed.
	void UnlinkVectorInputs(SActivationInfo* pActInfo)
	{
		IFlowGraph* pGraph = pActInfo->pGraph;
		std::vector<IFlowEdgeIterator::Edge> bad;
		IFlowEdgeIteratorPtr pIt = pGraph->CreateEdgeIterator();
		IFlowEdgeIterator::Edge edge;
		while (pIt->Next(edge))
		{
			if (edge.fromNodeId != pActInfo->myID || edge.fromPortId != 0)
				continue;
			const TFlowInputData* pData = pGraph->GetInputValue(edge.toNodeId, edge.toPortId);
			if (pData && pData->GetType() == eFDT_Vec3)
				bad.push_back(edge);
		}
		IEntity* pGraphEntity = gEnv->pEntitySystem->GetEntity(pGraph->GetGraphEntity(0));
		for (size_t i = 0; i < bad.size(); ++i)
		{
			pGraph->UnlinkNodes(SFlowAddress(bad[i].fromNodeId, bad[i].fromPortId, true), SFlowAddress(bad[i].toNodeId, bad[i].toPortId, false));
			CryLogAlways("[CoopFlow] %s: player id no longer fed into %s input %d (a position)",
				pGraphEntity ? pGraphEntity->GetName() : "<no entity>", pGraph->GetNodeTypeName(bad[i].toNodeId), (int)bad[i].toPortId);
		}
	}

public:
	EntityId GetOutput() const { return m_output; }

private:
	EntityId m_output;
	float m_timer;
	bool m_edgesChecked;
};

REGISTER_FLOW_NODE("Game:LocalPlayer", CFlowNode_CoopLocalPlayer);

// The player a level graph means. Single player and clients: the local
// player. Coop server: the player that graph's Game:LocalPlayer chose (the
// one closest to the graph's entity, i.e. who walked into the trigger), so
// Game:PlayerLink / Game:PlayerStaging act on the same player as the beams
// and dialogs of that graph, not always on the host.
CActor* CoopGraphPlayer(IFlowGraph* pGraph)
{
	IGameFramework* pFramework = g_pGame->GetIGameFramework();
	CActor* pLocal = static_cast<CActor*>(pFramework->GetClientActor());
	if (!pGraph || !gEnv->bServer || !CoopAI::IsCoopSession())
		return pLocal;
	IFlowNodeIteratorPtr pIt = pGraph->CreateNodeIterator();
	TFlowNodeId id;
	while (IFlowNodeData* pData = pIt->Next(id))
	{
		const char* type = pGraph->GetNodeTypeName(id);
		if (!type || stricmp(type, "Game:LocalPlayer"))
			continue;
		const EntityId playerId = static_cast<CFlowNode_CoopLocalPlayer*>(pData->GetNode())->GetOutput();
		if (IActor* pActor = playerId ? pFramework->GetIActorSystem()->GetActor(playerId) : 0)
			if (pActor->IsPlayer())
				return static_cast<CActor*>(pActor);
	}
	return pLocal;
}

// Mission:EndLevelNew replaces CryAction's node too (its configuration, so the
// ports are the same). CryAction's node ends the level by running the
// gamerules' EndLevel and then loading the next single player level itself;
// in coop that load closes the network session: the host ends up alone in the
// single player campaign and the other players are disconnected. On the coop
// server only the gamerules' EndLevel runs (it changes to the next coop map,
// the clients follow); on a coop client nothing happens (the server ends the
// level). Everywhere else an instance of CryAction's node does the work.
namespace
{
	IFlowNodePtr s_pOriginalEndLevel;
	IFlowNodePtr s_pOriginalSaveGame;

	IFlowNodePtr CaptureOriginal(IFlowSystem* pFlow, const char* type)
	{
		const TFlowNodeTypeId typeId = pFlow->GetTypeId(type);
		IFlowGraphPtr pGraph = typeId != InvalidFlowNodeTypeId ? pFlow->CreateFlowGraph() : IFlowGraphPtr();
		const TFlowNodeId id = pGraph ? pGraph->CreateNode(typeId, "CoopOriginal") : InvalidFlowNodeId;
		IFlowNodeData* pData = id != InvalidFlowNodeId ? pGraph->GetNodeData(id) : 0;
		IFlowNodePtr pNode = pData ? pData->GetNode() : 0;
		CryLogAlways("[CoopFlow] %s: CryAction's node %s", type, pNode ? "kept for single player" : "NOT FOUND");
		return pNode;
	}

	int FindInputPort(IFlowNode* pNode, const char* name)
	{
		SFlowNodeConfig config;
		pNode->GetConfiguration(config);
		for (int i = 0; config.pInputPorts && config.pInputPorts[i].name; ++i)
			if (!stricmp(config.pInputPorts[i].name, name))
				return i;
		return -1;
	}
}

void CoopCaptureOriginalFlowNodes(IFlowSystem* pFlow)
{
	s_pOriginalEndLevel = CaptureOriginal(pFlow, "Mission:EndLevelNew");
	s_pOriginalSaveGame = CaptureOriginal(pFlow, "System:SaveGame");
}

class CFlowNode_CoopEndLevel : public CFlowBaseNode
{
public:
	CFlowNode_CoopEndLevel(SActivationInfo* pActInfo)
	{
		if (s_pOriginalEndLevel)
			m_pOriginal = s_pOriginalEndLevel->Clone(pActInfo);
	}

	IFlowNodePtr Clone(SActivationInfo* pActInfo)
	{
		return new CFlowNode_CoopEndLevel(pActInfo);
	}

	void GetConfiguration(SFlowNodeConfig& config)
	{
		if (s_pOriginalEndLevel)
		{
			s_pOriginalEndLevel->GetConfiguration(config);
			return;
		}
		static const SInputPortConfig in_ports[] =
		{
			InputPortConfig_Void("Trigger", _HELP("Finish the current mission (go to next level)")),
			InputPortConfig<string>("NextLevel", _HELP("Which level is the next level?")),
			{0}
		};
		static const SOutputPortConfig out_ports[] =
		{
			{0}
		};
		config.pInputPorts = in_ports;
		config.pOutputPorts = out_ports;
		config.SetCategory(EFLN_APPROVED);
	}

	bool SerializeXML(SActivationInfo* pActInfo, const XmlNodeRef& root, bool reading)
	{
		return m_pOriginal ? m_pOriginal->SerializeXML(pActInfo, root, reading) : true;
	}

	void Serialize(SActivationInfo* pActInfo, TSerialize ser)
	{
		if (m_pOriginal)
			m_pOriginal->Serialize(pActInfo, ser);
	}

	void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo)
	{
		if (!CoopAI::IsCoopSession())
		{
			if (m_pOriginal)
				m_pOriginal->ProcessEvent(event, pActInfo);
			return;
		}
		if (event != eFE_Activate || !gEnv->bServer)
			return;
		SFlowNodeConfig config;
		GetConfiguration(config);
		int trigger = -1, next = -1;
		for (int i = 0; config.pInputPorts && config.pInputPorts[i].name; ++i)
			if (!stricmp(config.pInputPorts[i].name, "Trigger"))
				trigger = i;
			else if (!stricmp(config.pInputPorts[i].name, "NextLevel"))
				next = i;
		if (trigger < 0 || !IsPortActive(pActInfo, trigger))
			return;
		string nextLevel;
		if (next >= 0)
			pActInfo->pInputPorts[next].GetValueWithConversion(nextLevel);
		CGameRules* pGameRules = g_pGame->GetGameRules();
		IScriptTable* pScript = pGameRules ? pGameRules->GetEntity()->GetScriptTable() : 0;
		CryLogAlways("[CoopFlow] Mission:EndLevelNew: level end, next level '%s' (coop map change)", nextLevel.c_str());
		if (!pScript)
			return;
		SmartScriptTable params(gEnv->pScriptSystem);
		if (!nextLevel.empty())
			params->SetValue("nextlevel", nextLevel.c_str());
		Script::CallMethod(pScript, "EndLevel", params);
	}

	virtual void GetMemoryStatistics(ICrySizer* s)
	{
		s->Add(*this);
	}

private:
	IFlowNodePtr m_pOriginal;
};

REGISTER_FLOW_NODE("Mission:EndLevelNew", CFlowNode_CoopEndLevel);

// System:SaveGame (the campaign's checkpoints) replaces CryAction's node the
// same way. In coop CryAction's node does nothing (autosave is off: a single
// player save cannot be made or loaded from a network game as it is); on the
// coop server the checkpoint goes to CoopSave, which saves the progress.
class CFlowNode_CoopSaveGame : public CFlowBaseNode
{
public:
	CFlowNode_CoopSaveGame(SActivationInfo* pActInfo)
	{
		if (s_pOriginalSaveGame)
			m_pOriginal = s_pOriginalSaveGame->Clone(pActInfo);
	}

	IFlowNodePtr Clone(SActivationInfo* pActInfo)
	{
		return new CFlowNode_CoopSaveGame(pActInfo);
	}

	void GetConfiguration(SFlowNodeConfig& config)
	{
		if (s_pOriginalSaveGame)
		{
			s_pOriginalSaveGame->GetConfiguration(config);
			return;
		}
		static const SInputPortConfig in_ports[] =
		{
			InputPortConfig_Void("Save", _HELP("Save the game")),
			InputPortConfig_Void("Load", _HELP("Load the game")),
			InputPortConfig<string>("Name", _HELP("Name of SaveGame to save/load")),
			{0}
		};
		static const SOutputPortConfig out_ports[] =
		{
			{0}
		};
		config.pInputPorts = in_ports;
		config.pOutputPorts = out_ports;
		config.SetCategory(EFLN_APPROVED);
	}

	bool SerializeXML(SActivationInfo* pActInfo, const XmlNodeRef& root, bool reading)
	{
		return m_pOriginal ? m_pOriginal->SerializeXML(pActInfo, root, reading) : true;
	}

	void Serialize(SActivationInfo* pActInfo, TSerialize ser)
	{
		if (m_pOriginal)
			m_pOriginal->Serialize(pActInfo, ser);
	}

	void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo)
	{
		if (!CoopAI::IsCoopSession())
		{
			if (m_pOriginal)
				m_pOriginal->ProcessEvent(event, pActInfo);
			return;
		}
		if (event != eFE_Activate || !gEnv->bServer)
			return;
		const int save = FindInputPort(this, "Save"), name = FindInputPort(this, "Name");
		if (save < 0 || !IsPortActive(pActInfo, save))
			return;
		string checkpoint;
		if (name >= 0)
			pActInfo->pInputPorts[name].GetValueWithConversion(checkpoint);
		CoopSave::RequestCheckpoint(checkpoint.c_str());
	}

	virtual void GetMemoryStatistics(ICrySizer* s)
	{
		s->Add(*this);
	}

private:
	IFlowNodePtr m_pOriginal;
};

REGISTER_FLOW_NODE("System:SaveGame", CFlowNode_CoopSaveGame);
