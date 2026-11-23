# Third-party notices

AC-3DS links the following software into `assaultcube.3dsx`. The game media (maps, textures, models, sounds and configuration) is not part of this repository or of the release executable. Players supply it from the official AssaultCube package, which has its own terms ([AssaultCube licensing](upstream/license.html)).

| Component | License | Notice |
| --- | --- | --- |
| AssaultCube 1.3.0.2 source and the Cube engine | zlib-style, with the Cube clause that source versions may not be relicensed | [source/README.txt](../source/README.txt), [source/README_CUBEENGINE.txt](../source/README_CUBEENGINE.txt) |
| ENet 1.3.x | MIT | [source/enet/LICENSE](../source/enet/LICENSE) |
| SDL 2.32.10 | zlib | Copyright (C) 1997-2025 Sam Lantinga |
| SDL_image 2.8.8 | zlib | Copyright (C) 1997-2025 Sam Lantinga |
| stb_image (bundled with SDL_image) | Public domain or MIT, at your choice | Sean Barrett and contributors |
| libctru | zlib | Copyright (C) the libctru authors |
| Citro3D | zlib | Copyright (C) fincs and contributors |
| Tremor (libvorbisidec) and libogg | BSD 3-clause | Copyright (c) 2002-2020 Xiph.Org Foundation |
| zlib | zlib | Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler |
| Mbed TLS 2.28.8 (first-run download) | Apache-2.0 (offered as Apache-2.0 or GPL-2.0-or-later; used here under Apache-2.0) | Copyright The Mbed TLS Contributors. License text: <https://www.apache.org/licenses/LICENSE-2.0> |
| bzip2 / libbzip2 1.0.8 (first-run download) | bzip2 license (BSD-style) | Copyright (C) 1996-2019 Julian R Seward |

The first-run download also carries six public root certificates (USERTrust, Sectigo and ISRG) as trust anchors; they are listed in [source/3ds/firstrun_roots.h](../source/3ds/firstrun_roots.h). Mbed TLS and bzip2 are left out of a build configured with `-DAC3DS_FIRSTRUN=OFF`.

The one-file builder (`tools/builder/` and `scripts/make-3dsx.py`) contains no third-party code. Its bzip2 decoder, tar reader, SHA-256 and RomFS writer were written for this project; the RomFS and 3DSX layouts follow the formats that devkitPro's 3dsxtool writes and libctru reads, and no devkitPro tool is bundled. The Python script uses only the standard library.

## zlib license (SDL, SDL_image, libctru, Citro3D, zlib)

> This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## BSD 3-clause license (Tremor, libogg)

> Copyright (c) 2002-2020 Xiph.org Foundation
>
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
> - Neither the name of the Xiph.org Foundation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## MIT license (ENet, stb_image option A)

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## bzip2 license (libbzip2)

> This program, "bzip2", the associated library "libbzip2", and all documentation, are copyright (C) 1996-2019 Julian R Seward. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
> 2. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
> 3. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
> 4. The name of the author may not be used to endorse or promote products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Apache License 2.0 (Mbed TLS)

Mbed TLS is used unmodified, as built by devkitPro's `3ds-mbedtls` package, under the Apache License, Version 2.0. A copy of the license is at <https://www.apache.org/licenses/LICENSE-2.0>. Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
