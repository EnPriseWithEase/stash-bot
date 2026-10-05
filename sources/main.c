#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "hashkey.h"
#include "search.h"
#include "syncio.h"
#include "uci.h"

int main(int argc, char **argv) {
    sync_init();
    bitboard_init();
    zobrist_init();
    cyclic_init();
    search_init();

#ifndef TUNE
    uci_loop(argc, argv);
#else
    if (argc == 1) {
        printf("usage: %s dataset_file_1 [dataset_file_2 ...]\n", *argv);
        return 1;
    }

    TunerConfig config;
    TunerDataset dataset;

    tuner_config_set_default_values(&config);
    tuner_dataset_init(&dataset);

    for (int i = 1; i < argc; ++i) {
        tuner_dataset_add_file(&dataset, argv[i]);
    }

    tuner_dataset_start_session(&dataset, &config);
    tuner_dataset_destroy(&dataset);
#endif
    return 0;
}
