// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

namespace hl {

/// Signed 128-bit integer (GCC/Clang extension) used for overflow-free fixed-point products.
__extension__ typedef __int128 Int128;

}  // namespace hl
