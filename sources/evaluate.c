#include "evaluate.h"
#include <assert.h>

const Score PieceValue[PIECETYPE_NB] = {
    [NO_PIECETYPE] = 0,
    [PAWN]         = PAWN_VALUE,
    [KNIGHT]       = KNIGHT_VALUE,
    [BISHOP]       = BISHOP_VALUE,
    [ROOK]         = ROOK_VALUE,
    [QUEEN]        = QUEEN_VALUE,
    [KING]         = KING_VALUE
};

Score evaluate(const Board *board) {
    assert(board != NULL);
    assert(board->stack != NULL);

    Score score = board->stack->material[WHITE] - board->stack->material[BLACK];

    return (board->side_to_move == WHITE) ? score : -score;
}
