#include "search.h"
#include "board.h"
#include "evaluate.h"
#include "movepicker.h"
#include "tt.h"

// Piece values used for pure material calculations and futility margins
static const Score MaterialValue[7] = {
    0,     // NO_PIECE_TYPE
    100,   // PAWN
    300,   // KNIGHT
    300,   // BISHOP
    500,   // ROOK
    900,   // QUEEN
    0      // KING
};

Score qsearch(bool pv_node, Board *board, Score alpha, Score beta, Searchstack *ss) {
    Worker *worker = board_get_worker(board);
    const Score old_alpha = alpha;
    Movepicker mp;

    // Verify time usage if we are the main thread.
    if (worker->thread_index == 0) {
        wpool_check_time(worker->pool);
    }

    // Update seldepth if needed.
    if (pv_node) {
        worker->seldepth = u16_max(worker->seldepth, ss->plies + 1);
    }

    // Stop search if the game is drawn or search is stopped.
    if (wpool_is_stopped(worker->pool) || board_game_is_drawn(board, ss->plies)) {
        return worker_draw_score(worker);
    }

    // Stop search at MAX_PLIES limit.
    if (ss->plies >= MAX_PLIES) {
        return !board->stack->checkers ? evaluate(board) : worker_draw_score(worker);
    }

    // Mate distance pruning
    alpha = i16_max(alpha, mated_in(ss->plies));
    beta = i16_min(beta, mate_in(ss->plies + 1));

    if (alpha >= beta) {
        return alpha;
    }

    Score tt_score = NO_SCORE;
    Bound tt_bound = NO_BOUND;
    Move tt_move = NO_MOVE;
    bool tt_found;
    TranspositionEntry *tt_entry;

    tt_entry = tt_probe(&worker->pool->tt, board->stack->board_key, &tt_found);

    // Probe the Transposition Table.
    if (tt_found) {
        tt_score = score_from_tt(tt_entry->score, ss->plies);
        tt_bound = tt_entry_bound(tt_entry);
        tt_move = tt_entry->bestmove;

        if (!pv_node
            && (((tt_bound & LOWER_BOUND) && tt_score >= beta)
                || ((tt_bound & UPPER_BOUND) && tt_score <= alpha))) {
            return tt_score;
        }
    }

    const bool in_check = !!board->stack->checkers;
    Score raw_eval;
    Score eval;
    Score best_score;

    if (in_check) {
        raw_eval = eval = NO_SCORE;
        best_score = -INF_SCORE;
    } else {
        if (tt_found) {
            raw_eval = tt_entry->eval;
            eval = best_score = raw_eval;

            if (tt_bound & (tt_score > best_score ? LOWER_BOUND : UPPER_BOUND)) {
                best_score = tt_score;
            }
        } else {
            raw_eval = evaluate(board);
            eval = best_score = raw_eval;
        }

        // Stand Pat: allow direct cutoffs if static eval already beats beta.
        alpha = (Score)i16_max(alpha, best_score);

        if (alpha >= beta) {
            if (!tt_found) {
                tt_save(
                    &worker->pool->tt,
                    tt_entry,
                    board->stack->board_key,
                    score_to_tt(best_score, ss->plies),
                    raw_eval,
                    0,
                    LOWER_BOUND,
                    NO_MOVE
                );
            }
            return alpha;
        }
    }

    movepicker_init(&mp, true, board, worker, tt_move, ss);

    Move currmove;
    Move bestmove = NO_MOVE;
    i16 move_count = 0;
    Boardstack stack;

    const bool futility_ok = !in_check && board_total_piece_count(board) >= 5;
    const Score futility_base = best_score + 200;

    while ((currmove = movepicker_next_move(&mp, false, 0)) != NO_MOVE) {
        if (best_score > -MATE_FOUND && mp.stage == PICK_BAD_NOISY) {
            break;
        }

        if (!board_move_is_legal(board, currmove)) {
            continue;
        }

        ++move_count;
        const bool gives_check = board_move_gives_check(board, currmove);

        // Material Futility Pruning
        if (best_score > -MATE_FOUND && futility_ok && !gives_check
            && move_type(currmove) == NORMAL_MOVE) {

            Piecetype captured_type = piece_type(board_piece_on(board, move_to(currmove)));
            const Score futility_value = futility_base + MaterialValue[captured_type];

            if (futility_value < alpha) {
                best_score = i16_max(best_score, futility_value);
                continue;
            }

            if (futility_base < alpha && !board_see_above(board, currmove, 1)) {
                best_score = i16_max(best_score, futility_base);
                continue;
            }
        }

        ss->current_move = currmove;
        ss->piece_history = NULL;

        if (pv_node) {
            pv_line_init(&ss->pv);
        }

        board_do_move_gc(board, currmove, &stack, gives_check);
        prefetch(tt_entry_at(&worker->pool->tt, board->stack->board_key));
        worker_increment_nodes(worker);

        Score score = -qsearch(pv_node, board, -beta, -alpha, ss + 1);

        board_undo_move(board, currmove);

        if (wpool_is_stopped(worker->pool)) {
            return 0;
        }

        best_score = (Score)i16_max(best_score, score);

        if (alpha < best_score) {
            alpha = best_score;
            bestmove = currmove;

            if (pv_node && ss->plies > 0) {
                pv_line_update(&(ss - 1)->pv, bestmove, &ss->pv);
            }

            if (alpha >= beta) {
                break;
            }
        }
    }

    if (move_count == 0 && in_check) {
        best_score = mated_in(ss->plies);
    }

    Bound bound = (best_score >= beta) ? LOWER_BOUND
        : (best_score <= old_alpha)    ? UPPER_BOUND
                                       : EXACT_BOUND;

    tt_save(
        &worker->pool->tt,
        tt_entry,
        board->stack->board_key,
        score_to_tt(best_score, ss->plies),
        raw_eval,
        0,
        bound,
        bestmove
    );

    return best_score;
}

Score search(bool root_node, bool pv_node, Board *board, Score alpha, Score beta, i16 depth, Searchstack *ss, Move excluded_move) {
    Worker *worker = board_get_worker(board);
    const Score old_alpha = alpha;
    Movepicker mp;

    if (worker->thread_index == 0) {
        wpool_check_time(worker->pool);
    }

    if (pv_node) {
        worker->seldepth = u16_max(worker->seldepth, ss->plies + 1);
    }

    if (wpool_is_stopped(worker->pool) || board_game_is_drawn(board, ss->plies)) {
        return worker_draw_score(worker);
    }

    if (ss->plies >= MAX_PLIES) {
        return !board->stack->checkers ? evaluate(board) : worker_draw_score(worker);
    }

    if (depth <= 0) {
        return qsearch(pv_node, board, alpha, beta, ss);
    }

    // Mate Distance Pruning
    alpha = i16_max(alpha, mated_in(ss->plies));
    beta = i16_min(beta, mate_in(ss->plies + 1));
    if (alpha >= beta) {
        return alpha;
    }

    Score tt_score = NO_SCORE;
    Bound tt_bound = NO_BOUND;
    Move tt_move = NO_MOVE;
    bool tt_found;
    TranspositionEntry *tt_entry;

    uint64_t key = board->stack->board_key;
    if (excluded_move != NO_MOVE) {
        key ^= (uint64_t)excluded_move;
    }

    tt_entry = tt_probe(&worker->pool->tt, key, &tt_found);

    if (tt_found) {
        tt_score = score_from_tt(tt_entry->score, ss->plies);
        tt_bound = tt_entry_bound(tt_entry);
        tt_move = tt_entry->bestmove;

        if (!pv_node && tt_entry->depth >= depth
            && (((tt_bound & LOWER_BOUND) && tt_score >= beta)
                || ((tt_bound & UPPER_BOUND) && tt_score <= alpha))) {
            return tt_score;
        }
    }

    const bool in_check = !!board->stack->checkers;
    Score raw_eval;
    Score eval;
    Score best_score;

    if (in_check) {
        raw_eval = eval = NO_SCORE;
        best_score = -INF_SCORE;
    } else {
        if (tt_found) {
            raw_eval = tt_entry->eval;
            eval = best_score = raw_eval;

            if (tt_bound & (tt_score > best_score ? LOWER_BOUND : UPPER_BOUND)) {
                best_score = tt_score;
            }
        } else {
            raw_eval = evaluate(board);
            eval = best_score = raw_eval;
        }
    }

    movepicker_init(&mp, false, board, worker, tt_move, ss);

    Move currmove;
    Move bestmove = NO_MOVE;
    i16 move_count = 0;
    Boardstack stack;

    while ((currmove = movepicker_next_move(&mp, false, depth)) != NO_MOVE) {
        if (currmove == excluded_move) {
            continue;
        }

        if (!board_move_is_legal(board, currmove)) {
            continue;
        }

        ++move_count;
        const bool is_noisy = board_move_is_noisy(board, currmove);
        const bool gives_check = board_move_gives_check(board, currmove);

        ss->current_move = currmove;
        ss->piece_history = NULL;

        if (pv_node) {
            pv_line_init(&ss->pv);
        }

        board_do_move_gc(board, currmove, &stack, gives_check);
        prefetch(tt_entry_at(&worker->pool->tt, board->stack->board_key));
        worker_increment_nodes(worker);

        Score score;
        if (move_count == 1) {
            score = -search(false, pv_node, board, -beta, -alpha, depth - 1, ss + 1, NO_MOVE);
        } else {
            // PVS / LMR search
            i16 reduction = (move_count > 3 && depth >= 3 && !is_noisy && !gives_check) ? 1 : 0;
            score = -search(false, false, board, -alpha - 1, -alpha, depth - 1 - reduction, ss + 1, NO_MOVE);

            if (score > alpha && reduction > 0) {
                score = -search(false, false, board, -alpha - 1, -alpha, depth - 1, ss + 1, NO_MOVE);
            }

            if (score > alpha && score < beta) {
                score = -search(false, pv_node, board, -beta, -alpha, depth - 1, ss + 1, NO_MOVE);
            }
        }

        board_undo_move(board, currmove);

        if (wpool_is_stopped(worker->pool)) {
            return 0;
        }

        best_score = (Score)i16_max(best_score, score);

        if (alpha < best_score) {
            alpha = best_score;
            bestmove = currmove;

            if (pv_node && ss->plies > 0) {
                pv_line_update(&(ss - 1)->pv, bestmove, &ss->pv);
            }

            if (alpha >= beta) {
                if (!is_noisy) {
                    ss->killer = bestmove;
                }
                break;
            }
        }
    }

    if (move_count == 0) {
        if (excluded_move != NO_MOVE) {
            return alpha;
        }
        best_score = in_check ? mated_in(ss->plies) : worker_draw_score(worker);
    }

    Bound bound = (best_score >= beta) ? LOWER_BOUND
        : (best_score <= old_alpha)    ? UPPER_BOUND
                                       : EXACT_BOUND;

    if (!root_node || worker->pv_line == 0) {
        tt_save(
            &worker->pool->tt,
            tt_entry,
            key,
            score_to_tt(best_score, ss->plies),
            raw_eval,
            depth,
            bound,
            bestmove
        );
    }

    return best_score;
}

/* ========================================================================== */
/* Worker Thread & Search Initialization Entry Points                         */
/* ========================================================================== */

void search_init(void) {
    // Basic search setup if needed.
}

void main_worker_search(Worker *worker) {
    Searchstack stack[MAX_PLIES + 1];
    for (int i = 0; i <= MAX_PLIES; ++i) {
        stack[i].plies = i;
        stack[i].killer = NO_MOVE;
        stack[i].current_move = NO_MOVE;
        stack[i].piece_history = NULL;
        pv_line_init(&stack[i].pv);
    }

    worker->seldepth = 0;
    
    // Iterative Deepening Search Loop
    for (i16 depth = 1; depth <= MAX_PLIES; ++depth) {
        Score score = search(true, true, &worker->board, -INF_SCORE, INF_SCORE, depth, &stack[0], NO_MOVE);

        if (wpool_is_stopped(worker->pool)) {
            break;
        }

        (void)score;
    }
}

void worker_search(Worker *worker) {
    Searchstack stack[MAX_PLIES + 1];
    for (int i = 0; i <= MAX_PLIES; ++i) {
        stack[i].plies = i;
        stack[i].killer = NO_MOVE;
        stack[i].current_move = NO_MOVE;
        stack[i].piece_history = NULL;
        pv_line_init(&stack[i].pv);
    }

    // Helper Thread Iterative Deepening Loop
    for (i16 depth = 1; depth <= MAX_PLIES; ++depth) {
        search(true, false, &worker->board, -INF_SCORE, INF_SCORE, depth, &stack[0], NO_MOVE);

        if (wpool_is_stopped(worker->pool)) {
            break;
        }
    }
}
