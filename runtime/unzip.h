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

/* Any archive (a voice pack): the first file whose path ends with `suffix`
 * (whole path parts, any case) -- its full name into `name`, 1; else 0 */
int unzip_find(const uint8_t *zip, size_t n, const char *suffix, char *name, size_t namen);

/* every file, unpacked into buf (cap bytes) in turn and handed to fn (its name
 * is NOT NUL-terminated: nlen bytes); fn returns 0 to stop. Folders are
 * skipped. The number of files handed over, or -1 with the reason in err */
int unzip_each(const uint8_t *zip, size_t n, uint8_t *buf, size_t cap,
               int (*fn)(const char *name, size_t nlen, const uint8_t *data, size_t size, void *ud), void *ud,
               char *err, size_t errn);

#endif
