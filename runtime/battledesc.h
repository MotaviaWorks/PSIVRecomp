#ifndef PSIV_BATTLEDESC_H
#define PSIV_BATTLEDESC_H
/* one-line descriptions for the modern battle menus; NULL when there is none */
const char *battledesc_tech(int id);    /* techniques 1-40 */
const char *battledesc_skill(int id);   /* skills 1-54 */
const char *battledesc_item(int id);    /* items 1-255 */
#endif
