#include "board.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "attacks.h"
#include "bitboard.h"
#include "chess_types.h"
#include "core.h"
#include "evaluate.h"
#include "hashkey.h"
#include "movelist.h"
#include "syncio.h"
#include "wmalloc.h"

const StringView PieceIndexes = STATIC_STRVIEW(" PNBRQK  pnbrqk ");
const StringView PromotionIndexes = STATIC_STRVIEW("  nbrq  ");
const StringView StartposStr =
    STATIC_STRVIEW("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

Key CyclicKeys[0x2000];
Move CyclicMoves[0x2000];

static Bitboard board_attackers_to(const Board *board, Square square);
static Bitboard board_slider_blockers(
    const Board *restrict board,
    Bitboard sliders,
    Square square,
    Bitboard *restrict pinners
);

INLINED u16 cyclic_index_lo(Key key) {
    return key & 0x1FFFu;
}

INLINED u16 cyclic_index_hi(Key key) {
    return (key >> 13) & 0x1FFFu;
}

static void cyclic_init_move(Piece piece, Square from, Square to) {
    Move move = create_move(from, to);
    Key key = ZobristPsq[piece][from] ^ ZobristPsq[piece][to] ^ ZobristSideToMove;
    u16 index = cyclic_index_lo(key);

    while (move != NO_MOVE) {
        Key tmp_key = CyclicKeys[index];
        CyclicKeys[index] = key;
        key = tmp_key;

        Move tmp_move = CyclicMoves[index];
        CyclicMoves[index] = move;
        move = tmp_move;

        index ^= cyclic_index_lo(key) ^ cyclic_index_hi(key);
    }
}

void cyclic_init(void) {
    for (Piecetype piecetype = KNIGHT; piecetype <= KING; ++piecetype) {
        for (Color color = WHITE; color <= BLACK; ++color) {
            for (Square from = SQ_A1; from <= SQ_H8; ++from) {
                for (Square to = from + 1; to <= SQ_H8; ++to) {
                    if (bb_square_is_set(attacks_bb(piecetype, from, 0), to)) {
                        cyclic_init_move(create_piece(color, piecetype), from, to);
                    }
                }
            }
        }
    }
}

static void boardstack_set_check_info(Boardstack *restrict stack, const Board *restrict board) {
    const Square their_king_square = board_king_square(board, color_flip(board->side_to_move));

    stack->king_blockers[WHITE] = board_slider_blockers(
        board,
        board_color_bb(board, BLACK),
        board_king_square(board, WHITE),
        &stack->pinners[BLACK]
    );
    stack->king_blockers[BLACK] = board_slider_blockers(
        board,
        board_color_bb(board, WHITE),
        board_king_square(board, BLACK),
        &stack->pinners[WHITE]
    );

    stack->check_squares[PAWN] =
        pawn_attacks_bb(their_king_square, color_flip(board->side_to_move));
    stack->check_squares[KNIGHT] = knight_attacks_bb(their_king_square);
    stack->check_squares[BISHOP] = bishop_attacks_bb(their_king_square, board_occupancy_bb(board));
    stack->check_squares[ROOK] = rook_attacks_bb(their_king_square, board_occupancy_bb(board));
    stack->check_squares[QUEEN] = stack->check_squares[BISHOP] | stack->check_squares[ROOK];
    stack->check_squares[KING] = 0;
}

void boardstack_init(Boardstack *restrict stack, const Board *restrict board) {
    stack->board_key = 0;
    stack->checkers = board_attackers_to(board, board_king_square(board, board->side_to_move))
        & board_color_bb(board, color_flip(board->side_to_move));

    boardstack_set_check_info(stack, board);

    for (Bitboard bb = board_occupancy_bb(board); bb;) {
        const Square square = bb_pop_first_square(&bb);
        const Piece piece = board_piece_on(board, square);

        stack->board_key ^= ZobristPsq[piece][square];
    }

    if (stack->ep_square != SQ_NONE) {
        stack->board_key ^= ZobristEnPassant[square_file(stack->ep_square)];
    }

    if (board->side_to_move == BLACK) {
        stack->board_key ^= ZobristSideToMove;
    }

    stack->board_key ^= ZobristCastling[stack->castlings];
}

Boardstack *boardstack_clone(const Boardstack *stack) {
    if (stack == NULL) {
        return NULL;
    }

    Boardstack *const new_stack = wrap_malloc(sizeof(Boardstack));
    Boardstack *istack = new_stack;

    *istack = *stack;

    while (stack->previous != NULL) {
        istack->previous = wrap_malloc(sizeof(Boardstack));
        istack = istack->previous;
        stack = stack->previous;
        *istack = *stack;
    }

    istack->previous = NULL;
    return new_stack;
}

void boardstack_destroy(Boardstack *stack) {
    while (stack) {
        Boardstack *const prev = stack->previous;

        free(stack);
        stack = prev;
    }
}

static void board_put_piece(Board *board, Piece piece, Square square) {
    assert(piece_is_valid(piece));
    assert(square_is_valid(square));

    Bitboard sqbb = square_bb(square);

    board->mailbox[square] = piece;
    board->piecetype_bb[ALL_PIECES] |= sqbb;
    board->piecetype_bb[piece_type(piece)] |= sqbb;
    board->color_bb[piece_color(piece)] |= sqbb;
    ++board->piece_count[piece];
    ++board->piece_count[create_piece(piece_color(piece), ALL_PIECES)];
}

static void board_move_piece(Board *board, Square from, Square to) {
    assert(square_is_valid(from));
    assert(square_is_valid(to));
    assert(from != to);

    Piece piece = board_piece_on(board, from);
    Bitboard move_bb = square_bb(from) | square_bb(to);

    board->mailbox[from] = NO_PIECE;
    board->mailbox[to] = piece;
    board->piecetype_bb[ALL_PIECES] ^= move_bb;
    board->piecetype_bb[piece_type(piece)] ^= move_bb;
    board->color_bb[piece_color(piece)] ^= move_bb;
}

static void board_remove_piece(Board *board, Square square) {
    assert(square_is_valid(square));
    assert(!board_square_is_empty(board, square));

    Piece piece = board_piece_on(board, square);
    Bitboard notsq_bb = ~square_bb(square);

    board->mailbox[square] = NO_PIECE;
    board->piecetype_bb[ALL_PIECES] &= notsq_bb;
    board->piecetype_bb[piece_type(piece)] &= notsq_bb;
    board->color_bb[piece_color(piece)] &= notsq_bb;
    --board->piece_count[piece];
    --board->piece_count[create_piece(piece_color(piece), ALL_PIECES)];
}

static Bitboard board_attackers_list(const Board *board, Square square, Bitboard occupancy) {
    return (
        (pawn_attacks_bb(square, BLACK) & board_piece_bb(board, WHITE, PAWN))
        | (pawn_attacks_bb(square, WHITE) & board_piece_bb(board, BLACK, PAWN))
        | (knight_attacks_bb(square) & board_piecetype_bb(board, KNIGHT))
        | (bishop_attacks_bb(square, occupancy) & board_piecetypes_bb(board, BISHOP, QUEEN))
        | (rook_attacks_bb(square, occupancy) & board_piecetypes_bb(board, ROOK, QUEEN))
        | (king_attacks_bb(square) & board_piecetype_bb(board, KING))
    );
}

static Bitboard board_attackers_to(const Board *board, Square square) {
    assert(square_is_valid(square));
    return board_attackers_list(board, square, board_occupancy_bb(board));
}

static Bitboard board_slider_blockers(
    const Board *restrict board,
    Bitboard sliders,
    Square square,
    Bitboard *restrict pinners
) {
    Bitboard blockers = *pinners = 0;
    Bitboard snipers =
        ((rook_raw_attacks_bb(square) & board_piecetypes_bb(board, ROOK, QUEEN))
         | (bishop_raw_attacks_bb(square) & board_piecetypes_bb(board, BISHOP, QUEEN)))
        & sliders;
    const Bitboard occupancy = board_occupancy_bb(board) ^ snipers;

    while (snipers) {
        const Square sniper_square = bb_pop_first_square(&snipers);
        const Bitboard between = between_squares_bb(square, sniper_square) & occupancy;

        if (between && !bb_more_than_one(between)) {
            blockers |= between;
            if (between & board_color_bb(board, piece_color(board_piece_on(board, square)))) {
                bb_set_square(pinners, sniper_square);
            }
        }
    }

    return blockers;
}

static bool board_has_invalid_material(const Board *board, Color color) {
    const u8 knights = board_piece_count(board, create_piece(color, KNIGHT));
    const u8 bishops = board_piece_count(board, create_piece(color, BISHOP));
    const u8 rooks = board_piece_count(board, create_piece(color, ROOK));
    const u8 queens = board_piece_count(board, create_piece(color, QUEEN));
    u8 pawns = board_piece_count(board, create_piece(color, PAWN));

    const u8 pknights = u8_max(2, knights) - 2;
    const u8 pbishops = u8_max(2, bishops) - 2;
    const u8 prooks = u8_max(2, rooks) - 2;
    const u8 pqueens = u8_max(1, queens) - 1;
    const u8 promoted = pknights + pbishops + prooks + pqueens;

    pawns += promoted;
    return (pawns > 8) || (knights + pawns - pknights > 10) || (bishops + pawns - pbishops > 10)
        || (rooks + pawns - prooks > 10) || (queens + pawns - pqueens > 9);
}

static bool board_parse_fen_pieces(Board *board, StringView piece_field) {
    usize piece_index;
    File file = FILE_A;
    Rank rank = RANK_8;

    for (usize i = 0; i < piece_field.size; ++i) {
        u8 c = piece_field.data[i];

        if (c >= '1' && c <= '8') {
            file += c - '0';

            if (file > FILE_NB) {
                info_debug("info string Invalid FEN: too many squares in a single rank\n");
                return false;
            }
        } else if (c == '/') {
            if (file != FILE_NB) {
                info_debug("info string Invalid FEN: not enough squares in a single rank\n");
                return false;
            }

            if (rank == RANK_1) {
                info_debug("info string Invalid FEN: too many ranks\n");
                return false;
            }

            --rank;
            file = FILE_A;
        } else if (piece_index = strview_find(PieceIndexes, c), piece_index != NPOS) {
            if (!file_is_valid(file)) {
                info_debug("info string Invalid FEN: too many squares in a single rank\n");
                return false;
            }

            board_put_piece(board, (Piece)piece_index, create_square(file, rank));
            ++file;
        } else {
            info_debug(
                "info string Invalid FEN: encountered '%c' (0x%02hhx) while parsing the piece "
                "section\n",
                c,
                c
            );
            return false;
        }
    }

    if (rank != RANK_1) {
        info_debug("info string Invalid FEN: not enough ranks\n");
        return false;
    }

    if (file != FILE_NB) {
        info_debug("info string Invalid FEN: not enough squares in a single rank\n");
        return false;
    }

    if (board_piece_count(board, WHITE_KING) != 1 || board_piece_count(board, BLACK_KING) != 1) {
        info_debug("info string Invalid FEN: invalid number of kings\n");
        return false;
    }

    if (square_distance(board_king_square(board, WHITE), board_king_square(board, BLACK)) == 1) {
        info_debug("info string Invalid FEN: kings are next to each other\n");
        return false;
    }

    if (board_piecetype_bb(board, PAWN) & (RANK_1_BB | RANK_8_BB)) {
        info_debug("info string Invalid FEN: pawns on first/lask ranks\n");
        return false;
    }

    if (board_has_invalid_material(board, WHITE) || board_has_invalid_material(board, BLACK)) {
        info_debug("info string Invalid FEN: illegal material distribution\n");
        return false;
    }

    return true;
}

static bool board_parse_stm(Board *board, StringView stm_field) {
    if (stm_field.size > 1) {
        info_debug("info string Invalid FEN: the STM field cannot be more than one character\n");
        return false;
    }

    if (stm_field.size == 0 || stm_field.data[0] == 'w') {
        board->side_to_move = WHITE;
    } else if (stm_field.data[0] == 'b') {
        board->side_to_move = BLACK;
    } else {
        info_debug(
            "info string Invalid FEN: '%c' (0x%02hhx) is not a valid STM field\n",
            stm_field.data[0],
            stm_field.data[0]
        );
        return false;
    }

    return true;
}

static bool board_set_castling(Board *board, Color color, Square rook_square) {
    const Square king_square = board_king_square(board, color);
    const CastlingRight castling = relative_clright(color, king_square > rook_square);
    const CastlingMask castling_mask = clright_to_clmask(castling);

    if (square_rank_relative(king_square, color) != RANK_1) {
        info_debug("info string Invalid FEN: castling rights with the King not on its back rank\n");
        return false;
    }

    if (square_file(king_square) != FILE_E
        || (square_file(rook_square) != FILE_A && square_file(rook_square) != FILE_H)) {
        board->chess960 = true;
    }

    board->stack->castlings |= castling_mask;
    board->castling_mask[king_square] |= castling_mask;
    board->castling_mask[rook_square] |= castling_mask;
    board->castling_rook_square[castling] = rook_square;

    const Square king_after = square_relative((castling_mask & OO_MASK ? SQ_G1 : SQ_C1), color);
    const Square rook_after = square_relative((castling_mask & OO_MASK ? SQ_F1 : SQ_D1), color);

    board->castling_path[castling] =
        (between_squares_bb(rook_square, rook_after) | between_squares_bb(king_square, king_after)
         | square_bb(rook_after) | square_bb(king_after))
        & ~(square_bb(king_square) | square_bb(rook_square));

    return true;
}

static bool board_parse_castling(Board *board, StringView castling_field) {
    for (usize i = 0; i < castling_field.size; ++i) {
        if (castling_field.data[i] == '-') {
            if (castling_field.size > 1) {
                info_debug(
                    "info string Invalid FEN: '%.*s' is not a valid castling description\n",
                    (int)castling_field.size,
                    (const char *)castling_field.data
                );
                return false;
            }

            break;
        }

        const Color side = islower(castling_field.data[i]) ? BLACK : WHITE;
        const Piece rook = create_piece(side, ROOK);
        const u8 castling_char = toupper(castling_field.data[i]);
        Square rook_square;

        if (castling_char == 'K') {
            for (rook_square = square_relative(SQ_H1, side); square_file(rook_square) != FILE_A;
                 --rook_square) {
                if (board_piece_on(board, rook_square) == rook) {
                    break;
                }
            }
        } else if (castling_char == 'Q') {
            for (rook_square = square_relative(SQ_A1, side); square_file(rook_square) != FILE_H;
                 ++rook_square) {
                if (board_piece_on(board, rook_square) == rook) {
                    break;
                }
            }
        } else if (castling_char >= 'A' && castling_char <= 'H') {
            rook_square = create_square(castling_char - 'A', rank_relative(RANK_1, side));
        } else {
            info_debug(
                "info string Invalid FEN: '%c' (0x%02hhx) is not a valid castling character\n",
                castling_char,
                castling_char
            );
            return false;
        }

        if (!board_set_castling(board, side, rook_square)) {
            return false;
        }
    }

    return true;
}

static bool board_parse_en_passant(Board *board, StringView ep_field) {
    board->stack->ep_square = SQ_NONE;

    if (ep_field.size == 1 && ep_field.data[0] != '-') {
        info_debug(
            "info string Invalid FEN: '%c' is not a valid en passant description\n",
            ep_field.data[0]
        );
        return false;
    } else if (ep_field.size > 2) {
        info_debug(
            "info string Invalid FEN: the en passant field cannot have more than 2 characters\n"
        );
        return false;
    } else if (ep_field.size <= 1) {
        return true;
    }

    const u8 file_char = ep_field.data[0];
    const u8 rank_char = ep_field.data[1];

    if (file_char < 'a' || file_char > 'h'
        || rank_char != (board->side_to_move == WHITE ? '6' : '3')) {
        info_debug(
            "info string Invalid FEN: '%c%c' (0x%02hhx 0x%02hhx) is not a valid en passant "
            "square\n",
            file_char,
            rank_char,
            file_char,
            rank_char
        );
        return false;
    }

    const Square ep_square = create_square(file_char - 'a', rank_char - '1');
    const Color us = board->side_to_move;
    const Color them = color_flip(us);

    if (!bb_square_is_set(board_piece_bb(board, them, PAWN), ep_square + pawn_direction(them))) {
        info_debug(
            "info string Invalid FEN: en passant square defined with no enemy pawn occupying it\n"
        );
        return false;
    }

    if (pawn_attacks_bb(ep_square, them) & board_piece_bb(board, us, PAWN)) {
        board->stack->ep_square = ep_square;
    }

    return true;
}

static bool board_parse_rule50(Board *board, StringView rule50_field) {
    u64 value;

    if (rule50_field.size == 0) {
        board->stack->rule50 = 0;
        return true;
    }

    if (!strview_parse_u64(rule50_field, &value)) {
        info_debug(
            "info string Invalid FEN: '%.*s' is not a valid halfmove-clock value\n",
            (int)rule50_field.size,
            (const char *)rule50_field.data
        );
        return false;
    }

    if (value > 150) {
        info_debug(
            "info string Invalid FEN: the halfmove-clock cannot exceed 150 plies (75 moves)\n"
        );
        return false;
    }

    board->stack->rule50 = value;
    return true;
}

static bool board_parse_movenumber(Board *board, StringView movenumber_field) {
    u64 value;

    if (movenumber_field.size == 0) {
        board->ply = 0;
        return true;
    }

    if (!strview_parse_u64(movenumber_field, &value)) {
        info_debug(
            "info string Invalid FEN: '%.*s' is not a valid fullmove-number value\n",
            (int)movenumber_field.size,
            (const char *)movenumber_field.data
        );
        return false;
    }

    if (value > 8848) {
        info_debug("info string Invalid FEN: fullmove-number too large\n");
        return false;
    }

    board->ply = (u64_max(value, 1) - 1) * 2 + (board->side_to_move == BLACK);
    return true;
}

bool board_try_init(Board *board, StringView fen, bool is_chess960, Boardstack *stack) {
    memset(board, 0, sizeof(Board));
    memset(stack, 0, sizeof(Boardstack));
    board->stack = stack;

    if (!board_parse_fen_pieces(board, strview_next_word(&fen))) {
        return false;
    }

    if (!board_parse_stm(board, strview_next_word(&fen))) {
        return false;
    }

    if (board_attackers_to(board, board_king_square(board, color_flip(board->side_to_move)))
        & board_color_bb(board, board->side_to_move)) {
        info_debug("info string Invalid FEN: the opponent's King is in check\n");
        return false;
    }

    if (!board_parse_castling(board, strview_next_word(&fen))) {
        return false;
    }

    if (!is_chess960 && board->chess960) {
        info_debug("info string Warning: FRC position emitted with the UCI_Chess960 flag unset\n");
    }

    board->chess960 = board->chess960 || is_chess960;

    if (!board_parse_en_passant(board, strview_next_word(&fen))) {
        return false;
    }

    if (!board_parse_rule50(board, strview_next_word(&fen))) {
        return false;
    }

    if (!board_parse_movenumber(board, strview_next_word(&fen))) {
        return false;
    }

    boardstack_init(stack, board);

    if (bb_popcount(board->stack->checkers) > 2) {
        info_debug("info string Invalid FEN: more than 2 pieces are attacking the King\n");
        return false;
    }

    return true;
}

void board_clone(Board *restrict board, const Board *restrict other) {
    if (board->stack != NULL && board->stack != other->stack) {
        boardstack_destroy(board->stack);
        board->stack = NULL; // Fix: Clear stack pointer after destroy
    }

    *board = *other;
    board->has_worker = false;
    board->stack = boardstack_clone(other->stack);
}

static void barray_append_uint(u8 *buffer, usize *size, u64 value) {
    usize n = 0;

    do {
        buffer[*size + n] = (value % 10) + '0';
        value /= 10;
        ++n;
    } while (value != 0);

    for (usize i = 0; i < n / 2; ++i) {
        u8 tmp = buffer[*size + i];

        buffer[*size + i] = buffer[*size + n - i - 1];
        buffer[*size + n - i - 1] = tmp;
    }

    *size += n;
}

StringView board_get_fen(const Board *board) {
    static u8 fen_buffer[128];
    usize size = 0;

    for (Rank rank = RANK_8; rank_is_valid(rank); --rank) {
        for (File file = FILE_A; file <= FILE_H; ++file) {
            u8 empty_count = 0;

            while (file <= FILE_H && board_square_is_empty(board, create_square(file, rank))) {
                ++empty_count;
                ++file;
            }

            if (empty_count != 0) {
                fen_buffer[size++] = empty_count + '0';
            }

            if (file <= FILE_H) {
                fen_buffer[size++] =
                    PieceIndexes.data[board_piece_on(board, create_square(file, rank))];
            }
        }

        if (rank != RANK_1) {
            fen_buffer[size++] = '/';
        }
    }

    fen_buffer[size++] = ' ';
    fen_buffer[size++] = (board->side_to_move == WHITE) ? 'w' : 'b';
    fen_buffer[size++] = ' ';

    if (board->stack->castlings & WHITE_OO_MASK) {
        fen_buffer[size++] =
            board->chess960 ? 'A' + square_file(board->castling_rook_square[WHITE_OO]) : 'K';
    }

    if (board->stack->castlings & WHITE_OOO_MASK) {
        fen_buffer[size++] =
            board->chess960 ? 'A' + square_file(board->castling_rook_square[WHITE_OOO]) : 'Q';
    }

    if (board->stack->castlings & BLACK_OO_MASK) {
        fen_buffer[size++] =
            board->chess960 ? 'a' + square_file(board->castling_rook_square[BLACK_OO]) : 'k';
    }

    if (board->stack->castlings & BLACK_OOO_MASK) {
        fen_buffer[size++] =
            board->chess960 ? 'a' + square_file(board->castling_rook_square[BLACK_OOO]) : 'q';
    }

    if (!board->stack->castlings) {
        fen_buffer[size++] = '-';
    }

    fen_buffer[size++] = ' ';

    if (board->stack->ep_square == SQ_NONE) {
        fen_buffer[size++] = '-';
    }

    else {
        fen_buffer[size++] = 'a' + square_file(board->stack->ep_square);
        fen_buffer[size++] = '1' + square_rank(board->stack->ep_square);
    }

    fen_buffer[size++] = ' ';
    barray_append_uint(fen_buffer, &size, board->stack->rule50);
    fen_buffer[size++] = ' ';
    barray_append_uint(fen_buffer, &size, 1 + (board->ply - (board->side_to_move == BLACK)) / 2);
    return strview_from_raw_data(fen_buffer, size);
}

bool board_move_is_pseudolegal(const Board *board, Move move) {
    const Color us = board->side_to_move;
    const Color them = color_flip(board->side_to_move);
    const Square from = move_from(move);
    const Square to = move_to(move);
    const Piece piece = board_piece_on(board, from);

    if (move_type(move) != NORMAL_MOVE) {
        Movelist movelist;

        movelist_generate_pseudo(&movelist, board);
        return movelist_contains(&movelist, move);
    }

    if (move_promotion_type(move) != KNIGHT) {
        return false;
    }

    if (piece == NO_PIECE || piece_color(piece) != us) {
        return false;
    }

    if (bb_square_is_set(board_color_bb(board, us), to)) {
        return false;
    }

    if (piece_type(piece) == PAWN) {
        if (bb_square_is_set(RANK_1_BB | RANK_8_BB, to)) {
            return false;
        }

        if (!bb_square_is_set(pawn_attacks_bb(from, us) & board_color_bb(board, them), to)
            && !(from + pawn_direction(us) == to && board_square_is_empty(board, to))
            && !(
                from + 2 * pawn_direction(us) == to && square_rank_relative(from, us) == RANK_2
                && board_square_is_empty(board, to)
                && board_square_is_empty(board, to - pawn_direction(us))
            )) {
            return false;
        }
    }
    else if (!bb_square_is_set(
                 attacks_bb(piece_type(piece), from, board_occupancy_bb(board)),
                 to
             )) {
        return false;
    }

    if (board->stack->checkers) {
        if (piece_type(piece) != KING) {
            if (bb_more_than_one(board->stack->checkers)) {
                return false;
            }

            const Bitboard target = between_squares_bb(
                                        bb_first_square(board->stack->checkers),
                                        board_king_square(board, us)
                                    )
                | board->stack->checkers;

            if (!bb_square_is_set(target, to)) {
                return false;
            }
        }
        else if (!!(board_attackers_list(board, to, board_occupancy_bb(board) ^ square_bb(from))
                    & board_color_bb(board, them))) {
            return false;
        }
    }

    return true;
}

bool board_move_is_legal(const Board *board, Move move) {
    const Color us = board->side_to_move;
    const Color them = color_flip(board->side_to_move);
    const Square from = move_from(move);
    Square to = move_to(move);

    if (move_type(move) == EN_PASSANT) {
        const Square king_square = board_king_square(board, us);
        const Square capture_square = to - pawn_direction(us);
        const Bitboard occupancy_after =
            (board_occupancy_bb(board) ^ square_bb(from) ^ square_bb(capture_square))
            | square_bb(to);

        return !(bishop_attacks_bb(king_square, occupancy_after)
                 & board_pieces_bb(board, them, BISHOP, QUEEN))
            && !(
                rook_attacks_bb(king_square, occupancy_after)
                & board_pieces_bb(board, them, ROOK, QUEEN)
            );
    }

    if (move_type(move) == CASTLING) {
        to = square_relative(to > from ? SQ_G1 : SQ_C1, us);
        const Direction direction = to > from ? WEST : EAST;

        for (Square square = to; square != from; square += direction) {
            if (board_attackers_to(board, square) & board_color_bb(board, them)) {
                return false;
            }
        }

        return !board->chess960
            || !(
                rook_attacks_bb(to, board_occupancy_bb(board) ^ square_bb(move_to(move)))
                & board_pieces_bb(board, them, ROOK, QUEEN)
            );
    }

    if (piece_type(board_piece_on(board, from)) == KING) {
        return !(board_attackers_to(board, to) & board_color_bb(board, them));
    }

    return !bb_square_is_set(board->stack->king_blockers[us], from)
        || squares_are_aligned(from, to, board_king_square(board, us));
}

bool board_move_gives_check(const Board *board, Move move) {
    const Square from = move_from(move);
    const Square to = move_to(move);
    const Color us = board->side_to_move;
    const Color them = color_flip(board->side_to_move);
    const Square their_king = board_king_square(board, them);
    const Movetype movetype = move_type(move);

    if (bb_square_is_set(
            board->stack->check_squares[piece_type(board_piece_on(board, from))],
            to
        )) {
        return true;
    }

    if (bb_square_is_set(board->stack->king_blockers[them], from)
        && !squares_are_aligned(from, to, their_king)) {
        return true;
    }

    if (movetype == PROMOTION) {
        return bb_square_is_set(
            attacks_bb(move_promotion_type(move), to, board_occupancy_bb(board) ^ square_bb(from)),
            their_king
        );
    }

    if (movetype == EN_PASSANT) {
        const Square capture_square = create_square(square_file(to), square_rank(from));
        const Bitboard occupancy_after =
            (board_occupancy_bb(board) ^ square_bb(from) ^ square_bb(capture_square))
            | square_bb(to);

        return (bishop_attacks_bb(their_king, occupancy_after)
                & board_pieces_bb(board, us, BISHOP, QUEEN))
            || (rook_attacks_bb(their_king, occupancy_after)
                & board_pieces_bb(board, us, ROOK, QUEEN));
    }

    if (movetype == CASTLING) {
        const Square king_from = from;
        const Square rook_from = to;
        const Square king_to = square_relative(rook_from > king_from ? SQ_G1 : SQ_C1, us);
        const Square rook_to = square_relative(rook_from > king_from ? SQ_F1 : SQ_D1, us);

        if (!bb_square_is_set(rook_raw_attacks_bb(rook_to), their_king)) {
            return false;
        }

        return bb_square_is_set(
            rook_attacks_bb(
                rook_to,
                (board_occupancy_bb(board) ^ square_bb(king_from) ^ square_bb(rook_from))
                    | square_bb(king_to) | square_bb(rook_to)
            ),
            their_king
        );
    }

    return false;
}

static void board_do_castling(
    Board *restrict board,
    Color us,
    Square king_from,
    Square *restrict king_to,
    Square *restrict rook_from,
    Square *restrict rook_to
) {
    const bool kingside = *king_to > king_from;

    *rook_from = *king_to;
    *rook_to = square_relative(kingside ? SQ_F1 : SQ_D1, us);
    *king_to = square_relative(kingside ? SQ_G1 : SQ_C1, us);

    board_remove_piece(board, king_from);
    board_remove_piece(board, *rook_from);
    board_put_piece(board, create_piece(us, KING), *king_to);
    board_put_piece(board, create_piece(us, ROOK), *rook_to);
}

void board_do_move_gc(
    Board *restrict board,
    Move move,
    Boardstack *restrict new_stack,
    bool gives_check
) {
    const Color us = board->side_to_move;
    const Color them = color_flip(board->side_to_move);
    const Square from = move_from(move);
    Square to = move_to(move);
    const Piece piece = board_piece_on(board, from);
    Piece captured_piece =
        move_type(move) == EN_PASSANT ? create_piece(them, PAWN) : board_piece_on(board, to);
    Key key = board->stack->board_key ^ ZobristSideToMove;

    assert(piece_type(captured_piece) != KING);

    new_stack->castlings = board->stack->castlings;
    new_stack->rule50 = board->stack->rule50;
    new_stack->plies_since_nullmove = board->stack->plies_since_nullmove;
    new_stack->ep_square = board->stack->ep_square;
    new_stack->material_key = board->stack->material_key;
    new_stack->material[WHITE] = board->stack->material[WHITE];
    new_stack->material[BLACK] = board->stack->material[BLACK];

    board->ply += 1;
    new_stack->rule50 += 1;
    new_stack->plies_since_nullmove += 1;

    if (move_type(move) == CASTLING) {
        Square rook_from;
        Square rook_to;

        board_do_castling(board, us, from, &to, &rook_from, &rook_to);

        key ^= ZobristPsq[captured_piece][rook_from];
        key ^= ZobristPsq[captured_piece][rook_to];

        captured_piece = NO_PIECE;
    }

    if (captured_piece) {
        Square capture_square = to;

        if (move_type(move) == EN_PASSANT) {
            capture_square -= pawn_direction(us);
        }

        new_stack->material[them] -= PieceValue[piece_type(captured_piece)];
        board_remove_piece(board, capture_square);

        key ^= ZobristPsq[captured_piece][capture_square];
        new_stack->material_key ^= ZobristPsq[captured_piece][board->piece_count[captured_piece]];
        new_stack->rule50 = 0;
    }

    key ^= ZobristPsq[piece][from] ^ ZobristPsq[piece][to];

    if (new_stack->ep_square != SQ_NONE) {
        key ^= ZobristEnPassant[square_file(new_stack->ep_square)];
        new_stack->ep_square = SQ_NONE;
    }

    if (new_stack->castlings && (board->castling_mask[from] | board->castling_mask[to])) {
        const CastlingMask lost_castlings = board->castling_mask[from] | board->castling_mask[to];

        key ^= ZobristCastling[new_stack->castlings & lost_castlings];
        new_stack->castlings &= ~lost_castlings;
    }

    if (move_type(move) != CASTLING) {
        board_move_piece(board, from, to);
    }

    if (piece_type(piece) == PAWN) {
        new_stack->rule50 = 0;

        if ((to ^ from) == 16
            && (pawn_attacks_bb(to - pawn_direction(us), us) & board_piece_bb(board, them, PAWN))) {
            new_stack->ep_square = to - pawn_direction(us);
            key ^= ZobristEnPassant[square_file(new_stack->ep_square)];
        } else if (move_type(move) == PROMOTION) {
            const Piece new_piece = create_piece(us, move_promotion_type(move));

            board_remove_piece(board, to);
            board_put_piece(board, new_piece, to);

            key ^= ZobristPsq[piece][to] ^ ZobristPsq[new_piece][to];
            new_stack->material[us] += PieceValue[move_promotion_type(move)] - PieceValue[PAWN];
            new_stack->material_key ^= ZobristPsq[new_piece][board->piece_count[new_piece] - 1];
            new_stack->material_key ^= ZobristPsq[piece][board->piece_count[piece]];
        }
    }

    new_stack->captured_piece = captured_piece;
    new_stack->board_key = key;

    new_stack->checkers = gives_check
        ? board_attackers_to(board, board_king_square(board, them)) & board_color_bb(board, us)
        : 0;

    board->side_to_move = them;
    boardstack_set_check_info(new_stack, board);
    new_stack->repetition = 0;

    new_stack->previous = board->stack;

    const u16 repetition_plies = u16_min(new_stack->rule50, new_stack->plies_since_nullmove);

    if (repetition_plies >= 4) {
        Boardstack *rewind = new_stack->previous->previous;

        for (u16 i = 4; i <= repetition_plies; i += 2) {
            rewind = rewind->previous->previous;

            if (rewind->board_key == new_stack->board_key) {
                new_stack->repetition = rewind->repetition ? -(i16)i : (i16)i;
                break;
            }
        }
    }

    board->stack = new_stack;
}

void board_do_null_move(Board *restrict board, Boardstack *restrict new_stack) {
    memcpy(new_stack, board->stack, sizeof(Boardstack));

    new_stack->board_key ^= ZobristSideToMove;
    new_stack->rule50 += 1;
    new_stack->plies_since_nullmove = 0;

    if (new_stack->ep_square != SQ_NONE) {
        new_stack->board_key ^= ZobristEnPassant[square_file(new_stack->ep_square)];
        new_stack->ep_square = SQ_NONE;
    }

    board->side_to_move = color_flip(board->side_to_move);
    new_stack->repetition = 0;
    boardstack_set_check_info(new_stack, board);

    new_stack->previous = board->stack;
    board->stack = new_stack;
}

static void board_undo_castling(
    Board *restrict board,
    Color us,
    Square king_from,
    Square *restrict king_to
) {
    const bool kingside = *king_to > king_from;
    const Square rook_from = *king_to;
    const Square rook_to = square_relative(kingside ? SQ_F1 : SQ_D1, us);

    *king_to = square_relative(kingside ? SQ_G1 : SQ_C1, us);

    board_remove_piece(board, *king_to);
    board_remove_piece(board, rook_to);
    board_put_piece(board, create_piece(us, KING), king_from);
    board_put_piece(board, create_piece(us, ROOK), rook_from);
}

void board_undo_move(Board *board, Move move) {
    const Color us = color_flip(board->side_to_move);
    const Square from = move_from(move);
    Square to = move_to(move);

    board->side_to_move = us;

    if (move_type(move) == PROMOTION) {
        board_remove_piece(board, to);
        board_put_piece(board, create_piece(us, PAWN), to);
    }

    if (move_type(move) == CASTLING) {
        board_undo_castling(board, us, from, &to);
    } else {
        board_move_piece(board, to, from);

        if (board->stack->captured_piece) {
            Square capture_square = to;

            if (move_type(move) == EN_PASSANT) {
                capture_square -= pawn_direction(us);
            }

            board_put_piece(board, board->stack->captured_piece, capture_square);
        }
    }

    board->stack = board->stack->previous;
    board->ply -= 1;
}

void board_undo_null_move(Board *board) {
    board->stack = board->stack->previous;
    board->side_to_move = color_flip(board->side_to_move);
}

bool board_game_is_drawn(const Board *board, u16 ply) {
    if (!board || !board->stack) {
        return false;
    }

    if (board->stack->rule50 > 99) {
        if (!board->stack->checkers) {
            return true;
        }

        Movelist movelist;
        movelist_generate_legal(&movelist, board);

        if (movelist_size(&movelist) != 0) {
            return true;
        }
    }

    return board->stack->repetition && board->stack->repetition < (i16)ply;
}

bool board_game_contains_cycle(const Board *board, u16 ply) {
    const u16 max_plies = u16_min(board->stack->rule50, board->stack->plies_since_nullmove);
    const Key original_key = board->stack->board_key;
    const Boardstack *stack_it = board->stack->previous;

    if (max_plies < 3) {
        return false;
    }

    for (u16 i = 3; i <= max_plies; i += 2) {
        stack_it = stack_it->previous->previous;

        const Key move_key = original_key ^ stack_it->board_key;
        u16 index = cyclic_index_lo(move_key);

        if (CyclicKeys[index] != move_key) {
            index = cyclic_index_hi(move_key);

            if (CyclicKeys[index] != move_key) {
                continue;
            }
        }

        const Move move = CyclicMoves[index];
        const Square from = move_from(move);
        const Square to = move_to(move);

        if (between_squares_bb(from, to) & board_occupancy_bb(board)) {
            continue;
        }

        if (ply > i) {
            return true;
        }

        if (!stack_it->repetition) {
            continue;
        }

        if (piece_color(board_piece_on(board, board_square_is_empty(board, from) ? to : from))
            != board->side_to_move) {
            return true;
        }
    }

    return false;
}

bool board_see_above(const Board *board, Move move, Score threshold) {
    static const Score SeeScores[PIECETYPE_NB] = {
        0,
        PAWN_VALUE,
        KNIGHT_VALUE,
        BISHOP_VALUE,
        ROOK_VALUE,
        QUEEN_VALUE,
        0,
        0,
    };

    if (move_type(move) != NORMAL_MOVE && move_type(move) != PROMOTION) {
        return threshold <= 0;
    }

    const Square from = move_from(move);
    const Square to = move_to(move);
    Score next_value = SeeScores[piece_type(board_piece_on(board, to))] - threshold;

    if (next_value < 0) {
        return false;
    }

    next_value = SeeScores[piece_type(board_piece_on(board, from))] - next_value;

    if (next_value <= 0) {
        return true;
    }

    Bitboard occupancy = board_occupancy_bb(board) ^ square_bb(from) ^ square_bb(to);
    Color side_to_move = piece_color(board_piece_on(board, from));
    Bitboard attackers = board_attackers_list(board, to, occupancy);
    bool result = true;

    while (true) {
        side_to_move = color_flip(side_to_move);
        attackers &= occupancy;

        Bitboard stm_attackers = attackers & board_color_bb(board, side_to_move);
        Bitboard weakest_attackers;

        if (!stm_attackers) {
            break;
        }

        if (board->stack->pinners[color_flip(side_to_move)] & occupancy) {
            stm_attackers &= ~board->stack->king_blockers[side_to_move];

            if (!stm_attackers) {
                break;
            }
        }

        result = !result;

        if ((weakest_attackers = stm_attackers & board_piecetype_bb(board, PAWN))) {
            next_value = PAWN_VALUE - next_value;

            if (next_value < (Score)result) {
                break;
            }

            occupancy ^= weakest_attackers & -weakest_attackers;
            attackers |=
                bishop_attacks_bb(to, occupancy) & board_piecetypes_bb(board, BISHOP, QUEEN);
        } else if ((weakest_attackers = stm_attackers & board_piecetype_bb(board, KNIGHT))) {
            next_value = KNIGHT_VALUE - next_value;

            if (next_value < (Score)result) {
                break;
            }

            occupancy ^= weakest_attackers & -weakest_attackers;
        } else if ((weakest_attackers = stm_attackers & board_piecetype_bb(board, BISHOP))) {
            next_value = BISHOP_VALUE - next_value;

            if (next_value < (Score)result) {
                break;
            }

            occupancy ^= weakest_attackers & -weakest_attackers;
            attackers |=
                bishop_attacks_bb(to, occupancy) & board_piecetypes_bb(board, BISHOP, QUEEN);
        } else if ((weakest_attackers = stm_attackers & board_piecetype_bb(board, ROOK))) {
            next_value = ROOK_VALUE - next_value;

            if (next_value < (Score)result) {
                break;
            }

            occupancy ^= weakest_attackers & -weakest_attackers;
            attackers |= rook_attacks_bb(to, occupancy) & board_piecetypes_bb(board, ROOK, QUEEN);
        } else if ((weakest_attackers = stm_attackers & board_piecetype_bb(board, QUEEN))) {
            next_value = QUEEN_VALUE - next_value;

            if (next_value < (Score)result) {
                break;
            }

            occupancy ^= weakest_attackers & -weakest_attackers;
            attackers |=
                bishop_attacks_bb(to, occupancy) & board_piecetypes_bb(board, BISHOP, QUEEN);
            attackers |= rook_attacks_bb(to, occupancy) & board_piecetypes_bb(board, ROOK, QUEEN);
        } else {
            return (attackers & ~board_color_bb(board, side_to_move)) ? !result : result;
        }
    }

    return result;
}

StringView board_move_to_uci(const Board *board, Move move) {
    static u8 move_buffer[5];

    if (move == NO_MOVE) {
        memcpy(move_buffer, "none", 4);
        return strview_from_raw_data(move_buffer, 4);
    }

    if (move == NULL_MOVE) {
        memcpy(move_buffer, "0000", 4);
        return strview_from_raw_data(move_buffer, 4);
    }

    const Square from = move_from(move);
    Square to = move_to(move);

    if (move_type(move) == CASTLING && !board->chess960) {
        to = create_square(to > from ? FILE_G : FILE_C, square_rank(from));
    }

    move_buffer[0] = square_file(from) + 'a';
    move_buffer[1] = square_rank(from) + '1';
    move_buffer[2] = square_file(to) + 'a';
    move_buffer[3] = square_rank(to) + '1';

    if (move_type(move) == PROMOTION) {
        move_buffer[4] = " pnbrqk"[move_promotion_type(move)];
        return strview_from_raw_data(move_buffer, 5);
    }

    return strview_from_raw_data(move_buffer, 4);
}

static bool is_valid_file_ascii(u8 b) {
    return b >= 'a' && b <= 'h';
}

static bool is_valid_rank_ascii(u8 b) {
    return b >= '1' && b <= '8';
}

static bool board_move_tries_castling(const Board *board, Square from, Square to) {
    const Piece moving_piece = board_piece_on(board, from);

    if (piece_type(moving_piece) != KING) {
        return false;
    }

    if (board->chess960) {
        const Piece captured = board_piece_on(board, to);
        return piece_type(captured) == ROOK && piece_color(moving_piece) == piece_color(captured);
    }

    return square_file(from) == FILE_E && (square_file(to) == FILE_C || square_file(to) == FILE_G);
}

static bool board_move_tries_enpassant(const Board *board, Square from, Square to) {
    const Piece moving_piece = board_piece_on(board, from);

    if (board->stack->ep_square == SQ_NONE) {
        return false;
    }

    return piece_type(moving_piece) == PAWN && to == board->stack->ep_square;
}

Move board_uci_to_move(const Board *board, StringView move_strview) {
    if (move_strview.size != 4 && move_strview.size != 5) {
        return NO_MOVE;
    }

    const u8 from_file = move_strview.data[0];
    const u8 from_rank = move_strview.data[1];
    const u8 to_file = move_strview.data[2];
    const u8 to_rank = move_strview.data[3];

    if (!is_valid_file_ascii(from_file) || !is_valid_rank_ascii(from_rank)
        || !is_valid_file_ascii(to_file) || !is_valid_rank_ascii(to_rank)) {
        return NO_MOVE;
    }

    const Square from = create_square(from_file - 'a', from_rank - '1');
    const Square to = create_square(to_file - 'a', to_rank - '1');
    Move move;

    if (move_strview.size == 5) {
        u8 promo_char = move_strview.data[4];

        if (promo_char >= 'A' && promo_char <= 'Z') {
            promo_char = promo_char - 'A' + 'a';
        }

        if (promo_char < 'a' || promo_char > 'z') {
            return NO_MOVE;
        }

        usize promotion_index = strview_find(PromotionIndexes, promo_char);

        if (promotion_index == NPOS) {
            return NO_MOVE;
        }

        move = create_promotion_move(from, to, (Piece)promotion_index);
    } else if (board_move_tries_castling(board, from, to)) {
        Square king_to =
            board->chess960 ? to : create_square(from < to ? FILE_H : FILE_A, square_rank(to));

        move = create_castling_move(from, king_to);
    } else if (board_move_tries_enpassant(board, from, to)) {
        move = create_en_passant_move(from, to);
    } else {
        move = create_move(from, to);
    }

    if (!board_move_is_pseudolegal(board, move) || !board_move_is_legal(board, move)) {
        return NO_MOVE;
    }

    return move;
}
