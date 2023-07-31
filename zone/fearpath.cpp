#include "../common/rulesys.h"

#include "map.h"
#include "pathfinder_interface.h"
#include "mob_movement_manager.h"
#include "string_ids.h"
#include "water_map.h"
#include "zone.h"

#ifdef _WINDOWS
#define snprintf _snprintf
#endif

extern Zone *zone;

#define FEAR_PATHING_DEBUG

int Mob::GetFleeRatio(Mob *other) {
	int specialFleeRatio = GetSpecialAbility(FLEE_PERCENT);
	if (specialFleeRatio > 0) {
		return specialFleeRatio;
	}

	int fleeRatio = RuleI(Combat, FleeHPRatio);

	Mob *hate_top = GetHateTop();
	if (other != nullptr)
		hate_top = other;

	if (!hate_top) {
		return 0;
	}

	uint8 hateTopLevel = hate_top->GetLevel();
	if (GetLevel() <= hateTopLevel) {
		if (hate_top->GetLevelCon(GetLevel()) == CON_GREEN && GetLevel() <= DEEP_GREEN_LEVEL) {
			// green con 18 and under runs much earlier
			return 50;
		}
	} else {
		if (GetLevel() > (hateTopLevel + 2)) {
			// red con
			return fleeRatio / 2;
		}
	}

	return fleeRatio;
}

// this is called whenever we are damaged to process possible fleeing
void Mob::CheckFlee() {
	if (IsPet() || IsCasting() || (IsNPC() && CastToNPC()->IsUnderwaterOnly()))
		return;

	// if were already fleeing, we only need to check speed.  Speed changes will trigger pathing updates.
	if (flee_mode && curfp) {
		float flee_speed = GetFearSpeed();
		if (flee_speed < 0.1f)
			flee_speed = 0.0f;
		SetRunAnimation(flee_speed);
		if (IsMoving() && flee_speed < 0.1f)
			StopNavigation();
		return;
	}

	// dont bother if we are immune to fleeing
	if (GetSpecialAbility(IMMUNE_FLEEING))
		return;

	// see if were possibly hurt enough
	float ratio = GetHPRatio();
	float fleeratio = static_cast<float>(GetFleeRatio());

	if (ratio > fleeratio)
		return;

	// hp cap so 1 million hp NPCs don't flee with 200,000 hp left
	if (!GetSpecialAbility(FLEE_PERCENT) && GetHP() > 15000)
		return;

	// we might be hurt enough, check con now..
	Mob *hate_top = GetHateTop();
	if (!hate_top) {
		// this should never happen...
		StartFleeing();
		return;
	}

	float other_ratio = hate_top->GetHPRatio();
	if (other_ratio <= 20) {
		// our hate top is almost dead too... stay and fight
		return;
	}

	if (RuleB(Combat, FleeIfNotAlone) ||
	    GetSpecialAbility(ALWAYS_FLEE) ||
	    (GetSpecialAbility(ALWAYS_FLEE_LOW_CON) && hate_top->GetLevelCon(GetLevel()) == CON_GREEN) ||
	    (!RuleB(Combat, FleeIfNotAlone) && entity_list.FleeAllyCount(hate_top, this) == 0)) {
		StartFleeing();
	}
}

void Mob::StopFleeing() {
	if (!flee_mode)
		return;

	flee_mode = false;

	// see if we are legitimately feared or blind now
	if (!IsFearedNoFlee() && !IsBlind()) {
		curfp = false;
		StopNavigation();
	}
}

void Mob::FleeInfo(Mob *client) {
	float other_ratio = client->GetHPRatio();
	bool wontflee = false;
	std::string reason;
	std::string flee;

	int allycount = entity_list.FleeAllyCount(client, this);

	if (flee_mode && curfp) {
		wontflee = true;
		reason = "NPC is already fleeing!";
	} else if (GetSpecialAbility(IMMUNE_FLEEING)) {
		wontflee = true;
		reason = "NPC is immune to fleeing.";
	} else if (other_ratio < 20) {
		wontflee = true;
		reason = "Player has low health.";
	} else if (GetSpecialAbility(ALWAYS_FLEE)) {
		flee = "NPC has ALWAYS_FLEE set.";
	} else if (GetSpecialAbility(ALWAYS_FLEE_LOW_CON) && client->GetLevelCon(GetLevel()) == CON_GREEN) {
		flee = "NPC has ALWAYS_FLEE_LOW_CON and is green to the player.";
	} else if (RuleB(Combat, FleeIfNotAlone) || (!RuleB(Combat, FleeIfNotAlone) && allycount == 0)) {
		flee = "NPC has no allies nearby or the rule to flee when not alone is enabled.";
	} else {
		wontflee = true;
		reason = "NPC likely has allies nearby.";
	}

	if (!wontflee) {
		client->Message(CC_Green, "%s will flee at %d percent because %s", GetName(), GetFleeRatio(client), flee.c_str());
	} else {
		client->Message(CC_Red, "%s will not flee because %s", GetName(), reason.c_str());
	}

	client->Message(CC_Default, "NPC ally count %d", allycount);
}

void Mob::ProcessFlee() {
	if (!flee_mode)
		return;

	// Stop fleeing if effect is applied after they start to run.
	// When ImmuneToFlee effect fades it will turn fear back on and check if it can still flee.
	//  Stop flee if we've become a pet after we began fleeing.
	if (flee_mode && (GetSpecialAbility(IMMUNE_FLEEING) || IsCharmedPet()) && !IsFearedNoFlee() && !IsBlind()) {
		curfp = false;
		return;
	}

	bool dying = GetHPRatio() < GetFleeRatio();
	// We have stopped fleeing for an unknown reason (couldn't find a node is possible) restart.
	if (flee_mode && !curfp) {
		if (dying)
			StartFleeing();
	}

	// see if we are still dying, if so, do nothing
	if (dying)
		return;

	// we are not dying anymore, check to make sure we're not blind or feared and cancel flee.
	StopFleeing();
}

void Mob::CalculateNewFearpoint() {
	// blind waypoint logic isn't the same as fear's.  Has a chance to run toward the player
	// chance is very high if the player is moving, otherwise it's low
	if (IsBlind() && !IsFeared() && GetTarget()) {
		int roll = 20;
		if (GetTarget()->GetCurrentSpeed() > 0.1f || (GetTarget()->IsClient() && GetTarget()->animation != 0))
			roll = 80;

		if (zone->random.Roll(roll)) {
			m_FearWalkTarget = glm::vec3(GetTarget()->GetPosition());
			curfp = true;
			return;
		}
	}

	if (RuleB(Pathing, Fear) && zone->pathing) {
		auto Node = zone->pathing->GetRandomLocation(glm::vec3(GetX(), GetY(), GetZ()), PathingNotDisabled ^ PathingZoneLine);
		if (Node.x != 0.0f || Node.y != 0.0f || Node.z != 0.0f) {
			Node.z = GetFixedZ(Node);
			PathfinderOptions opts;
			opts.smooth_path = true;
			opts.step_size = RuleR(Pathing, NavmeshStepSize);
			opts.offset = GetZOffset();
			opts.flags = PathingNotDisabled ^ PathingZoneLine;
			auto partial = false;
			auto stuck = false;
			auto route = zone->pathing->FindPath(
			    glm::vec3(GetX(), GetY(), GetZ()),
			    glm::vec3(Node.x, Node.y, Node.z),
			    partial,
			    stuck,
			    opts);
			if (stuck) {
				curfp = false;
			} else if (route.size() > 2 || CheckLosFN(Node.x, Node.y, Node.z, 6.0)) {
				// iterate the route, to make sure no LOS failures
				auto iter = route.begin();
				glm::vec3 previous_pos(GetX(), GetY(), GetZ());
				bool have_los = true;
				while (iter != route.end() && have_los == true) {
					auto &current_node = (*iter);
					iter++;

					if (iter == route.end()) {
						continue;
					}

					previous_pos = current_node.pos;
					auto &next_node = (*iter);

					if (next_node.teleport)
						continue;

					if (!zone->zonemap->CheckLoS(previous_pos, next_node.pos))
						have_los = false;
				}
				if (have_los) {
					m_FearWalkTarget = Node;
					curfp = true;
					return;
				}
			}
		}
	}

	bool inliquid = zone->HasWaterMap() && zone->watermap->InLiquid(glm::vec3(GetPosition())) || zone->IsWaterZone(GetZ());
	bool stay_inliquid = (inliquid && IsNPC() && CastToNPC()->IsUnderwaterOnly());
	bool levitating = IsClient() && (FindType(SE_Levitate) || flymode != EQ::constants::GravityBehavior::Ground);

	int loop = 0;
	float ranx, rany, ranz;
	curfp = false;
	glm::vec3 myloc(GetX(), GetY(), GetZ());
	glm::vec3 myceil = myloc;
	float ceil = zone->zonemap->FindCeiling(myloc, &myceil);
	if (ceil != BEST_Z_INVALID) {
		ceil -= 1.0f;
	}
	while (loop < 100)  // Max 100 tries
	{
		int ran = 250 - (loop * 2);
		loop++;
		ranx = GetX() + zone->random.Int(0, ran - 1) - zone->random.Int(0, ran - 1);
		rany = GetY() + zone->random.Int(0, ran - 1) - zone->random.Int(0, ran - 1);
		ranz = BEST_Z_INVALID;
		glm::vec3 newloc(ranx, rany, ceil != BEST_Z_INVALID ? ceil : GetZ());

		if (stay_inliquid || levitating) {
			if (zone->zonemap->CheckLoS(myloc, newloc)) {
				ranz = GetZ();
				curfp = true;
				break;
			}
		} else {
			if (ceil != BEST_Z_INVALID)
				ranz = zone->zonemap->FindGround(newloc, &myceil);
			else
				ranz = zone->zonemap->FindBestZ(newloc, &myceil);
			if (ranz != BEST_Z_INVALID)
				ranz = SetBestZ(ranz);
		}
		if (ranz == BEST_Z_INVALID)
			continue;
		float fdist = ranz - GetZ();
		if (fdist >= -50 && fdist <= 50 && CheckCoordLosNoZLeaps(GetX(), GetY(), GetZ(), ranx, rany, ranz)) {
			curfp = true;
			break;
		}
	}
	if (curfp)
		m_FearWalkTarget = glm::vec3(ranx, rany, ranz);
}
