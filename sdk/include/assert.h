/*
 * assert(): a failed check prints the file and line and ends the program
 * with status 134, like abort(). Define NDEBUG to leave checks out.
 */
#ifndef OPOS_ASSERT_H
#define OPOS_ASSERT_H

__attribute__((noreturn)) void opos_assert_failed(const char *expression,
                                                  const char *file,
                                                  int line);

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                \
    ((expression) ? (void)0                                               \
                  : opos_assert_failed(#expression, __FILE__, __LINE__))
#endif

#ifndef __cplusplus
#define static_assert _Static_assert
#endif

#endif
