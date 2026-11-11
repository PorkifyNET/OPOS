#ifndef OPOS_ACCOUNTS_H
#define OPOS_ACCOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * User accounts, stored in /disk/OPOS/ACCOUNTS.DAT on the mounted disk.
 * Each account has a name, an optional password (salted, iterated
 * SHA-256), an administrator flag, its personal settings, and a home
 * folder /disk/Users/<name> with Documents, Pictures, Music, Videos and
 * Downloads. The kernel enforces location-based access to home folders.
 */

#define ACCOUNT_MAX 8
#define ACCOUNT_NAME_MAX 20
#define ACCOUNT_PASSWORD_MAX 32
#define ACCOUNT_NONE ((size_t)-1)
#define ACCOUNT_HOME_ROOT "/disk/Users"

enum account_result {
    ACCOUNT_OK,
    ACCOUNT_BAD_NAME,
    ACCOUNT_NAME_TAKEN,
    ACCOUNT_FULL,
    ACCOUNT_NOT_FOUND,
    ACCOUNT_LAST_ADMIN,
    ACCOUNT_DISK_ERROR,
};

/* Reloads the account list from the mounted disk (none without a disk). */
void accounts_load(void);
size_t accounts_count(void);
const char *accounts_name(size_t index);
bool accounts_has_password(size_t index);
bool accounts_is_admin(size_t index);
/* Checks a password without changing the logged-in account. */
bool accounts_password_matches(size_t index, const char *password);
/* Index of the account with this name (case-insensitive), or ACCOUNT_NONE. */
size_t accounts_find(const char *name);

/* True when a login screen is needed: several accounts, or a password. */
bool accounts_login_required(void);
/* Checks the password (ignored for accounts without one) and logs in. */
bool accounts_login(size_t index, const char *password);
void accounts_logout(void);
/* Logged-in account, or ACCOUNT_NONE. */
size_t accounts_current(void);

/* Writes "/disk/Users/<name>" plus an optional "/sub" folder. Returns the
   length, or 0 when nobody is logged in or it does not fit. */
size_t accounts_home_path(const char *sub, char *path, size_t capacity);

bool accounts_name_valid(const char *name);
const char *accounts_result_text(enum account_result result);
/* Creates the account and its home folders. An empty password means none. */
enum account_result accounts_create(const char *name, const char *password,
                                    bool admin);
enum account_result accounts_set_password(size_t index, const char *password);
/* Renames the account and its home folder. */
enum account_result accounts_rename(size_t index, const char *name);
enum account_result accounts_set_admin(size_t index, bool admin);
/* Removes the account; its home folder and files are kept. */
enum account_result accounts_delete(size_t index);

/* Personal settings bits (see SETTINGS_PERSONAL_MASK in cmos.h) of the
   logged-in account; false when nobody is logged in. */
bool accounts_personal_settings(uint8_t *flags);
void accounts_set_personal_settings(uint8_t flags);

#endif
