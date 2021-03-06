#ifndef BAUDOT_H
#define BAUDOT_H

extern const int charset_count;

char asc2baudot(int asc, bool figs);
char baudot2asc(int baudot, bool figs);
const char *charset_name(void);

#endif
