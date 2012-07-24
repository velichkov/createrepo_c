/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#ifndef __C_CREATEREPOLIB_CONSTANTS_H__
#define __C_CREATEREPOLIB_CONSTANTS_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Checksum type.
 */
typedef enum {
    CR_CHECKSUM_MD5,           /*!< MD5 checksum */
    CR_CHECKSUM_SHA1,          /*!< SHA1 checksum */
    CR_CHECKSUM_SHA256         /*!< SHA256 checksum */
} cr_ChecksumType;

#ifdef __cplusplus
}
#endif

#endif /* __C_CREATEREPOLIB_CONSTANTS_H__ */
