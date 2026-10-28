#ifndef LOG_H
#define LOG_H

enum log_level { L_INFO, L_WARN, L_ERROR };

/* Writes "LEVEL tag: message" to stderr. Preserves errno. */
void log_msg(enum log_level lvl, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#endif
