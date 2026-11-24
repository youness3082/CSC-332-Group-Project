# loganalyzer — command 5
Read a large regular log file efficiently with `mmap()` and count lines, severity keywords and optional literal keyword matches.

```bash
make
make test
printf 'INFO started\nWARN low\nERROR failed\n' > sample.log
./loganalyzer -k WARN -v sample.log
```

Uses `open`, `fstat`, `mmap`, `munmap`, `close`, `getopt`, and `clock_gettime`. Counts each category once per line, including mixed-category lines. A file ending with a newline does not add an extra blank line. Empty files work. It does not parse timestamps or infer semantic log levels beyond literal uppercase matches.
