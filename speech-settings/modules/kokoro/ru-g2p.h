/*
 * ru-g2p - Russian text to IPA for the Russian Kokoro (zaakirio/kokoro-ru)
 *
 * A port of its ru_g2p.py (Apache-2.0): stress marks (U+0301), orthoepic
 * respelling, espeak-ng with an acute-aware ru_dict, IPA normalization,
 * vowel reduction.  RUAccent is replaced by the stress dictionary of
 * Vosk TTS (word<TAB>phonemes, sorted by bytes, the stressed vowel has "1").
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef RU_G2P_H
#define RU_G2P_H

#include <glib.h>

typedef struct RuG2p RuG2p;

/* dict: sorted Vosk dictionary (NULL: no stress marks);
 * espeak_path: the directory that holds espeak-ng-data with the acute-aware
 * ru_dict.  espeak-ng is global, so only one RuG2p per process. */
RuG2p *ru_g2p_new (const gchar *dict, const gchar *espeak_path, GError **error);
void ru_g2p_free (RuG2p *g);

/* text with U+0301 after the stressed vowels (ё restored) */
gchar *ru_g2p_accentuate (RuG2p *g, const gchar *text);
/* IPA from text that already carries the stress marks */
gchar *ru_g2p_phonemize_marked (RuG2p *g, const gchar *marked);
/* accentuate + phonemize_marked */
gchar *ru_g2p_phonemize (RuG2p *g, const gchar *text);

#endif
