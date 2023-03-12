/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BUG_H
#define _LINUX_BUG_H

#define BUG_ON(__BUG_ON_cond) assert(!(__BUG_ON_cond))

#define BUILD_BUG_ON(x)

#define BUG() abort()

#endif /* _LINUX_BUG_H */
