/*
 * commands.h - entry points of the team commands linked into the Custom Shell.
 *
 * Each command's source file is compiled with -D<NAME>_NO_MAIN, which hides
 * its own main() and leaves only the <name>_main() function below.
 */
#ifndef COMMANDS_H
#define COMMANDS_H

int iotracker_main(int argc, char **argv);         /* Command 1 */
int filecrypt_main(int argc, char **argv);         /* Command 3 */
int processgroup_main(int argc, char **argv);      /* Command 4 */
int loganalyzer_main(int argc, char **argv);       /* Command 5 */
int sharedmempipe_main(int argc, char **argv);     /* Command 6 */
int filediffadvanced_main(int argc, char **argv);  /* Command 8 */

#endif
