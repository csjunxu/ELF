#include "default_policy.h"
// #include "pattern.h"
#include "assert.h"

DefPolicy::DefPolicy() {
    // Set default parameters.
    for (int i = 0; i < NUM_MOVE_TYPE; ++i) switches_[i] = true;

    // Allow self-atari moves for groups with <= 3 stones.
    thres_allow_atari_stone_ = 3;

    // By default save all atari stones.
    thres_save_atari_ = 1;

    // Attack opponent groups with 1 libs or less, with 1 or more stones (i.e., any group).
    thres_opponent_libs_ = 1;
    thres_opponent_stones_ = 1;
}

void DefPolicy::PrintParams() {
    for (int i = 0; i < NUM_MOVE_TYPE; ++i) {
        printf("%d: %s\n", i, (switches_[i] ? "true" : "false"));
    }
}

void DefPolicy::compute_policy(DefPolicyMoves *m, const Region *r) {
    // Initialize moves.
    m->clear();

    if (switches_[KO_FIGHT]) check_ko_fight(m, r);
    if (switches_[OPPONENT_IN_DANGER]) check_opponent_in_danger(m, r);
    if (switches_[OUR_ATARI]) check_our_atari(m, r);
    if (switches_[NAKADE]) check_nakade(m, r);
    if (switches_[PATTERN]) check_pattern(m, r);
}

bool DefPolicy::sample(DefPolicyMoves *ms, RandFunc rand_func, bool verbose, GroupId4 *ids, DefPolicyMove *m) {
    assert(ms);

    if (ms->size() == 0) return false;

    int count = 0;
    while (1) {
        // Sample distribution.
        int i = ms->sample(rand_func);
        if (i < 0) return false;

        if (verbose) {
            printf("Sample step = %d\n", count);
            ms->PrintInfo(i);
        }

        if (ids == NULL || TryPlay2(ms->board(), ms->at(i).m, ids)) {
            *m = ms->at(i);
            return true;
        }
        // Otherwise set gamma to be zero and redo this.
        ms->remove(i);
        count ++;
    }
}

bool DefPolicy::simple_sample(const DefPolicyMoves *ms, RandFunc rand_func, GroupId4 *ids, DefPolicyMove *m) {
    assert(ms);
    if (ms->size() == 0) return false;

    int i = rand_func() % ms->size();
    if (ids == NULL || TryPlay2(ms->board(), ms->at(i).m, ids)) {
        *m = ms->at(i);
        return true;
    }
    return false;
}


// Utilities for playing default policy. Referenced from Pachi's code.
void DefPolicy::check_ko_fight(DefPolicyMoves *, const Region *) {
    // Need to implement ko age.
    /*
       if (GetSimpleKoLocation(board) != M_PASS) {
       }
       */
}

// Get the move with specific structure.
Coord DefPolicy::get_moves_from_group(DefPolicyMoves *m, unsigned char id, MoveType type) {
    const Board *board = m->board();
    // Find the atari point.
    int count = 0;
    int lib_count = board->_groups[id].liberties;
    Coord last = M_PASS;
    TRAVERSE(board, id, c) {
        FOR4(c, _, cc) {
            if (board->_infos[cc].color == S_EMPTY) {
                if (m != nullptr) m->add(DefPolicyMove(cc, type));
                last = cc;
                count ++;
                if (count == lib_count) break;
            }
        } ENDFOR4
        if (count == lib_count) break;
    } ENDTRAVERSE
    return last;
}

// Check any of the opponent group has lib <= lib_thres, if so, make all the capture moves.
void DefPolicy::check_opponent_in_danger(DefPolicyMoves *m, const Region *r) {
    const Board *board = m->board();
    // Loop through all groups and check.
    // Group id starts from 1.
    Stone opponent = OPPONENT(board->_next_player);
    for (int i = 1; i < board->_num_groups; ++i) {
        const Group* g = &board->_groups[i];
        if (g->color != opponent) continue;
        // If the liberties of opponent group is too many, skip.
        if (g->liberties > thres_opponent_libs_) continue;
        // If #stones of opponent group is too few, skip.
        if (g->stones < thres_opponent_stones_) continue;
        if (!GroupInRegion(board, i, r)) continue;

        // Find the intersections that reduces its point and save it to the move queue.
        get_moves_from_group(m, i, OPPONENT_IN_DANGER);
        // ShowBoard(board, SHOW_LAST_MOVE);
        // util_show_move(m, board->_next_player);
    }
}

// Check any of our group has lib = 1, if so, try saving it.
void DefPolicy::check_our_atari(DefPolicyMoves *m, const Region *r) {
    const Board *board = m->board();

    for (int i = 1; i < board->_num_groups; ++i) {
        const Group* g = &board->_groups[i];
        // If the group is not in atari or its #stones are not too many, skip.
        if (g->color != board->_next_player || g->liberties != 1 || g->stones < thres_save_atari_) continue;
        if (!GroupInRegion(board, i, r)) continue;

        // Find the atari point.
        Coord c = get_moves_from_group(nullptr, i, NORMAL);
        if (c == M_PASS) {
            char buf[30];
            printf("Cannot get the atari point for group %d start with %s!\n", i, get_move_str(g->start, S_EMPTY, buf));
            ShowBoard(board, SHOW_ALL);
            DumpBoard(board);
            error("");
        }
        // If that point has liberty (or connected with another self group with > 2 liberty), take the move.
        int liberty = 0;
        int group_rescue = 0;
        FOR4(c, _, cc) {
            const Info* info = &board->_infos[cc];
            if (info->color == S_EMPTY) {
                liberty ++;
            } else if (info->color == board->_next_player) {
                if (board->_groups[info->id].liberties > 2) group_rescue ++;
            }
        } ENDFOR4
        if (liberty > 0 || group_rescue > 0) {
            m->add(DefPolicyMove(c, OUR_ATARI));
        }
    }
}

// Get Nakade point, refactored from Pachi: pachi/tactics/nakade.c
// The goal is to find the nakade point to kill the opponent in the next move.
// [TODO]: We also need to enforce our own nakade point.
static Coord nakade_point(const Board *board, Coord loc) {
    /* First, examine the nakade area. For sure, it must be at most
     * six points. And it must be within color group(s). */
#define NAKADE_MAX 6
    Coord area[NAKADE_MAX]; int area_n = 0;

    area[area_n++] = loc;

    // Simple flood fill to find the region.
    // printf("Flood fill...\n");
    for (int i = 0; i < area_n; i++) {
        FOR4(area[i], _, c) {
            // If that point is surrounding by our stone, return immediately.
            if (board->_infos[c].color == board->_next_player) return M_PASS;
            if (board->_infos[c].color != S_EMPTY) continue;
            bool dup = false;
            for (int j = 0; j < area_n; j++)
                if (c == area[j]) {
                    dup = true;
                    break;
                }
            if (dup) continue;

            if (area_n >= NAKADE_MAX) {
                /* Too large nakade area. */
                return M_PASS;
            }
            area[area_n++] = c;
        } ENDFOR4
    }

    /* We also collect adjecency information - how many neighbors
     * we have for each area point, and histogram of this. This helps
     * us verify the appropriate bulkiness of the shape. */
    // Compute a few statistics.
    // printf("Compute a few statistics...\n");
    int neighbors[area_n]; int ptbynei[9] = {area_n, 0};
    memset(neighbors, 0, sizeof(neighbors));
    for (int i = 0; i < area_n; i++) {
        for (int j = i + 1; j < area_n; j++)
            if (NEIGHBOR4(area[i], area[j])) {
                ptbynei[neighbors[i]]--;
                neighbors[i]++;
                ptbynei[neighbors[i]]++;
                ptbynei[neighbors[j]]--;
                neighbors[j]++;
                ptbynei[neighbors[j]]++;
            }
    }

    /* For each given neighbor count, arbitrary one coordinate
     * featuring that. */

    // printf("Anchor coordinate...\n");
    Coord coordbynei[9];
    for (int i = 0; i < area_n; i++)
        coordbynei[neighbors[i]] = area[i];

    // printf("Determine the type\n");
    switch (area_n) {
        case 1: return M_PASS;
        case 2: return M_PASS;
        case 3: // assert(ptbynei[2] == 1);
                return coordbynei[2]; // middle point
        case 4: if (ptbynei[3] != 1) return M_PASS; // long line
                    return coordbynei[3]; // tetris four
        case 5: if (ptbynei[3] == 1 && ptbynei[1] == 1) return coordbynei[3]; // bulky five
                    if (ptbynei[4] == 1) return coordbynei[4]; // cross five
                return M_PASS; // long line
        case 6: if (ptbynei[4] == 1 && ptbynei[2] == 3)
                    return coordbynei[4]; // rabbity six
                return M_PASS; // anything else

    }

    printf("This should never happen!");
    return M_PASS;
}

// Check if there is any nakade point, if so, play it to kill the opponent's group.
void DefPolicy::check_nakade(DefPolicyMoves *m, const Region *r) {
    const Board *board = m->board();
    Coord empty = M_PASS;
    if (board->_last_move == M_PASS) return;
    if (r != nullptr && ! IsIn(r, board->_last_move)) return;

    FOR4(board->_last_move, _, c) {
        if (board->_infos[c].color != S_EMPTY) continue;
        if (empty == M_PASS) {
            empty = c;
            continue;
        }
        if (!NEIGHBOR8(c, empty)) {
            /* Seems like impossible nakade
             * shape! */
            return;
        }
    } ENDFOR4

    if (empty != M_PASS) {
        Coord nakade = nakade_point(board, empty);
        if (nakade != M_PASS) {
            m->add(DefPolicyMove(nakade, NAKADE));
        }
    }
}

// Check the 3x3 pattern matcher
void DefPolicy::check_pattern(DefPolicyMoves *, const Region *) {
}

// The default policy
DefPolicyMove DefPolicy::Run(RandFunc rand_func, Board* board, const Region *r, int max_depth, bool verbose) {
    AllMoves all_moves;
    GroupId4 ids;
    char buf[30];
    int num_pass = 0;

    DefPolicyMove move;
    DefPolicyMoves m(board);

    if (verbose) {
        printf("Start default policy!\n");
    }
    // If max_depth is < 0, run the default policy until the end of game.
    if (max_depth < 0) max_depth = 10000000;

    for (int k = 0; k < max_depth; ++k) {
        if (verbose) {
            // printf("Default policy: k = %d/%d, player = %d\n", k, max_depth, player);
            ShowBoard(board, SHOW_ALL);
            // DumpBoard(board);
        }

        // Utilities for playing default policy. Referenced from Pachi's code.
        // printf("Start computing def policy\n");
        compute_policy(&m, r);

        // printf("Start sampling def policy\n");
        bool sample_res = sample(&m, rand_func, verbose, &ids, &move);
        int idx = -1;

        if (! sample_res) {
            // Fall back to the normal mode.
            if (verbose) printf("Before find all valid moves..\n");
            FindAllCandidateMovesInRegion(board, r, board->_next_player, thres_allow_atari_stone_, &all_moves);
            if (verbose) printf("After find all valid moves..\n");
            if (all_moves.num_moves == 0) {
                // No move to play, just pass.
                move.m = M_PASS;
            } else {
                // Sample one move
                idx = rand_func() % all_moves.num_moves;
                move.m = all_moves.moves[idx];
            }

            move.type = NORMAL;
            move.gamma = 0;
            if (! TryPlay2(board, move.m, &ids)) {
                printf("\n\n#candidate moves: %d. Current move idx: %d\n", all_moves.num_moves, idx);
                for (int i = 0; i < all_moves.num_moves; ++i) {
                    printf("%s ", get_move_str(all_moves.moves[i], board->_next_player, buf));
                }
                printf("\n");
                printf("Move: x = %d, y = %d, str = %s\n", X(move.m), Y(move.m), get_move_str(move.m, board->_next_player, buf));
                printf("Move (from board) = %s\n", get_move_str(move.m, board->_next_player, buf));
                ShowBoard(board, SHOW_ALL);
                printf("[%d/%d]: Move cannot be executed!", k, max_depth);
                throw std::range_error("Move cannot be executed!");
            }
        }

        if (verbose) {
            util_show_move(move.m, board->_next_player, buf);
        }

        // Keep playing (even if the game already end by two pass or a resign), until we see a consecutive two passes.
        Play(board, &ids);

        // Check if there is any consecutive two passes.
        if (move.m == M_PASS) {
            num_pass ++;
            if (num_pass == 2) break;
        }
        else num_pass = 0;
    }

    if (verbose) {
        printf("Finish default policy!\n");
    }

    if (num_pass == 2) move.game_ended = true;
    return move;
}
