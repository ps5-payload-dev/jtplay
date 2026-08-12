#!/usr/bin/env python3
# Copyright (C) 2026 John Törnblom
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.

import argparse
import glob
import mimetypes
import string


INC_MACRO = '''
#define INCASSET(name, file)                                     \\
  __asm__(                                                       \\
      ".section .rodata\\n"                                      \\
      ".global " #name "\\n"                                     \\
      ".global " #name "_end\\n"                                 \\
      ".global " #name "_size\\n"                                \\
      ".align 16\\n"                                             \\
      #name ":\\n"                                               \\
      ".incbin \\"" #file "\\"\\n"                               \\
      #name "_end:\\n"                                           \\
      #name "_size:\\n"                                          \\
      ".quad " #name "_end - " #name "\\n"                       \\
      ".previous\\n"                                             \\
  );                                                             \\
  extern const uint8_t name[];                                   \\
  extern const size_t name##_size;
'''

def find_files(path):
    for name in glob.glob(path + '/**', recursive=True):
        if not mimetypes.guess_type(name)[0]:
            continue
        yield name


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('PATH')
    args = parser.parse_args()

    assets = list(sorted(find_files(args.PATH)))

    print(INC_MACRO)
    for ind, name in enumerate(assets):
        print(f'INCASSET(file{ind}, {name});')

    print('\n__attribute__((constructor)) static void')
    print('constructor(void) {')

    for ind, name in enumerate(assets):
        mime = mimetypes.guess_type(name)[0]
        name = name[6:]
        print(f'  asset_register("{name}", file{ind}, file{ind}_size, "{mime}");');

    print('}\n')

