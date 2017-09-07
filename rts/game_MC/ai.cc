/**
* Copyright (c) 2017-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "ai.h"
#include "engine/game_env.h"
#include "engine/unit.h"
#include "engine/cmd_interface.h"

static inline int sampling(const std::vector<float> &v, std::mt19937 *gen) {
    std::vector<float> accu(v.size() + 1);
    std::uniform_real_distribution<> dis(0, 1);
    float rd = dis(*gen);

    accu[0] = 0;
    for (size_t i = 1; i < accu.size(); i++) {
        accu[i] = v[i - 1] + accu[i - 1];
        if (rd < accu[i]) {
            return i - 1;
        }
    }

    return v.size() - 1;
}

static inline void accu_value(int idx, float val, std::map<int, std::pair<int, float> > &idx2record) {
    auto it = idx2record.find(idx);
    if (it == idx2record.end()) {
        idx2record.insert(make_pair(idx, make_pair(1, val)));
    } else {
        it->second.second += val;
        it->second.first ++;
    }
}

void AIBase::save_structured_state(const GameEnv &env, Data *data) const {
    GameState *game = &data->newest();
    game->tick = _receiver->GetTick();
    game->winner = env.GetWinnerId();
    game->terminal = env.GetTermination() ? 1 : 0;
    game->player_name = _name;

    // Extra data.
    game->ai_start_tick = 0;

    compute_state(env, &game->s);

    game->last_r = 0.0;
    int winner = env.GetWinnerId();

    if (winner != INVALID) {
        if (winner == _player_id) game->last_r = 1.0;
        else game->last_r = -1.0;
    }
}

void AIBase::compute_state(const GameEnv &env, std::vector<float> *state) const {
    const int n_type = env.GetGameDef().GetNumUnitType();
    const int n_additional = 2;
    const int resource_grid = 50;
    const int res_pt = NUM_RES_SLOT;
    const int total_channel = n_type + n_additional + res_pt;

    const auto &m = env.GetMap();

    // [Channel, width, height]
    const int sz = total_channel * m.GetXSize() * m.GetYSize();
    state->resize(sz);
    std::fill(state->begin(), state->end(), 0.0);

    std::map<int, std::pair<int, float> > idx2record;

#define _OFFSET(_c, _x, _y) (((_c) * m.GetYSize() + (_y)) * m.GetXSize() + (_x))

    PlayerId visibility_check = _respect_fow ? _player_id : INVALID;

    auto unit_iter = env.GetUnitIterator(visibility_check);
    float total_hp_ratio = 0.0;

    int myworker = 0;
    int mytroop = 0;
    int mybarrack = 0;

    std::vector<int> quantized_r(env.GetNumOfPlayers(), 0);

    while (! unit_iter.end()) {
        const Unit &u = *unit_iter;
        int x = int(u.GetPointF().x);
        int y = int(u.GetPointF().y);
        float hp_level = u.GetProperty()._hp / (u.GetProperty()._max_hp + 1e-6);
        UnitType t = u.GetUnitType();

        bool self_unit = (u.GetPlayerId() == _player_id);

        accu_value(_OFFSET(t, x, y), 1.0, idx2record);

        // Self unit or enemy unit.
        // For historical reason, the flag of enemy unit = 2
        accu_value(_OFFSET(n_type, x, y), (self_unit ? 1 : 2), idx2record);
        accu_value(_OFFSET(n_type + 1, x, y), hp_level, idx2record);

        total_hp_ratio += hp_level;

        if (self_unit) {
            if (t == WORKER) myworker += 1;
            else if (t == MELEE_ATTACKER || t == RANGE_ATTACKER) mytroop += 1;
            else if (t == BARRACKS) mybarrack += 1;
       }

        ++ unit_iter;
    }

    for (const auto &p : idx2record) {
        state->at(p.first) = p.second.second / p.second.first;
    }

    myworker = min(myworker, 3);
    mytroop = min(mytroop, 5);
    mybarrack = min(mybarrack, 1);

    for (int i = 0; i < env.GetNumOfPlayers(); ++i) {
        // Omit player signal from other player's perspective.
        if (visibility_check != INVALID && visibility_check != i) continue;
        const auto &player = env.GetPlayer(i);
        quantized_r[i] = min(int(player.GetResource() / resource_grid), res_pt - 1);
    }

    if (_player_id != INVALID) {
        // Add resource layer for the current player.
        const int c = _OFFSET(n_type + n_additional + quantized_r[_player_id], 0, 0);
        std::fill(state->begin() + c, state->begin() + c + m.GetXSize() * m.GetYSize(), 1.0);
    }
}


bool TrainedAI2::on_act(const GameEnv &env) {
    _state.resize(NUM_AISTATE);
    std::fill (_state.begin(), _state.end(), 0);

    // if (_receiver == nullptr) return false;
    Tick t = _receiver->GetTick();

    if (! this->need_structured_state(t)) {
        // We just use the backup AI.
        // cout << "Use the backup AI, tick = " << _receiver->GetTick() << endl;
        SendComment("BackupAI");
        return _backup_ai->Act(env);
    }
    // Get the current action from the queue.
    int num_action = NUM_AISTATE;
    int h = num_action - 1;
    const GameState& gs = _ai_comm->info().data.newest();
    // uint64_t hash_code;

    switch(gs.action_type) {
        case ACTION_GLOBAL:
            // action
            h = gs.a;
            _state[h] = 1;

            // hash_code = serializer::hash_obj(*_ai_comm->GetData());
            // cout << "[" << t << "]: hash = " << hex << hash_code << dec << ", h = " << h << endl;
            return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
                return _mc_rule_actor.ActByState(e, _state, s, assigned_cmds);
            });

        case ACTION_PROB:
            // parse probablity
            // [TODO] This code is really not good, need refactoring.
            {
              if (_receiver->GetUseCmdComment()) {
                string s;
                for (int i = 0; i < NUM_AISTATE; ++i) {
                    s += to_string(gs.pi[i]) + ",";
                }
                SendComment(s);
              }

              h = sampling(gs.pi, &_ai_comm->gen());
              _state[h] = 1;
              return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
                  return _mc_rule_actor.ActByState(e, _state, s, assigned_cmds);
              });
            }
        case ACTION_UNIT_CMD:
            {
                // Make it vector of CmdInput.
                _cmd_inputs.clear();
                for (int i = 0; i < gs.n_max_cmd; ++i) {
                    PointF unit_loc(gs.unit_loc[2*i], gs.unit_loc[2*i+1]);
                    PointF target_loc(gs.target_loc[2*i], gs.target_loc[2*i+1]);
                    auto t = (CmdInput::CmdInputType)(gs.cmd_type[i]);
                    auto build_type = (UnitType)(gs.build_type[i]);

                    // Check unit id.
                    UnitId id = env.GetMap().GetClosestUnitId(unit_loc, 1.0);
                    UnitId target_id = env.GetMap().GetClosestUnitId(target_loc, 1.0);
                    UnitId base = INVALID;

                    if (t == CmdInput::CI_GATHER) base = env.FindClosestBase(Player::ExtractPlayerId(id));

                    _cmd_inputs.emplace_back(t, id, target_loc, target_id, base, build_type);
                } 

                return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
                        return _mc_rule_actor.ActByCmd(e, _cmd_inputs, s, assigned_cmds);
                });
            }

            /*
        case ACTION_REGIONAL:
            {
              if (_receiver->GetUseCmdComment()) {
                string s;
                for (size_t i = 0; i < gs.a_region.size(); ++i) {
                  for (size_t j = 0; j < gs.a_region[0].size(); ++j) {
                    int a = -1;
                    for (size_t k = 0; k < gs.a_region[0][0].size(); ++k) {
                      if (gs.a_region[k][i][j] == 1) {
                        a = k;
                        break;
                      }
                    }
                    s += to_string(a) + ",";
                  }
                  s += "; ";
                }
                SendComment(s);
              }
            }
            // Regional actions.
            return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
                return _mc_rule_actor.ActWithMap(e, reply.action_regions, s, assigned_cmds);
            });
            */
        default:
            throw std::range_error("action_type not valid! " + to_string(gs.action_type));
    }
}

///////////////////////////// Simple AI ////////////////////////////////
bool SimpleAI::on_act(const GameEnv &env) {
    _state.resize(NUM_AISTATE, 0);
    std::fill (_state.begin(), _state.end(), 0);
    return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
        _mc_rule_actor.GetActSimpleState(&_state);
        return _mc_rule_actor.ActByState(e, _state, s, assigned_cmds);
    });
}

bool HitAndRunAI::on_act(const GameEnv &env) {
    _state.resize(NUM_AISTATE, 0);
    std::fill (_state.begin(), _state.end(), 0);
    return gather_decide(env, [&](const GameEnv &e, string *s, AssignedCmds *assigned_cmds) {
        _mc_rule_actor.GetActHitAndRunState(&_state);
        return _mc_rule_actor.ActByState(e, _state, s, assigned_cmds);
    });
}
