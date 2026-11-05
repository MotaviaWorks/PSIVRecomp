/*
 * The ROM out of a .zip: most ROM sets are zipped, one game per archive
 * (No-Intro: "Phantasy Star IV (USA).zip" holding a .md). Reads the archive's
 * directory, takes the first Mega Drive ROM-like entry (.md .bin .gen .smd
 * .68k) and unpacks it: stored, or deflated (RFC 1951). No zip64, no
 * encryption -- a ROM archive needs neither.
 */
#ifndef PSIV_UNZIP_H
#define PSIV_UNZIP_H

#include <stddef.h>
#include <stdint.h>

int unzip_is_zip(const uint8_t *data, size_t n);

/* the ROM entry unpacked into out (at most cap bytes): its size, or 0 with
 * the reason in err */
size_t unzip_rom(const uint8_t *zip, size_t n, uint8_t *out, size_t cap, char *err, size_t errn);

#endif
