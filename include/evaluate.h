#ifndef EVALUATE_H
#define EVALUATE_H

#include "board.h"
#include "chess_types.h"

// Basic piece values for material balance, incremental board tracking, and SEE
#define PAWN_VALUE   100
#define KNIGHT_VALUE 300
#define BISHOP_VALUE 300
#define ROOK_VALUE   500
#define QUEEN_VALUE  900
#define KING_VALUE   20000

// Piece value lookup table shared across board.c and evaluate.c
extern const Score PieceValue[PIECETYPE_NB];

// Evaluates the current board state strictly based on material balance.
// Returns a score from the perspective of the side to move.
Score evaluate(const Board *board);

#endif // EVALUATE_H
