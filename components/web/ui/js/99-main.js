/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Entry point: theme first (no flash of the wrong colours), then the
 * session decides between setup, log in, public status and the app.
 */

applyTheme();
boot();
