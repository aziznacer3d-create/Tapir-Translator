Tapir Translator - third-party components embedded in Tapir_Translator.apx
===========================================================================

Tapir Translator itself is in the public domain (Unlicense, LICENSE file of the repository).
The components below are under free (permissive) licenses: none is non-commercial nor
share-alike. They only require their text to be included, which is what this folder does.

Component                         Version / origin                                    License
--------------------------------  --------------------------------------------------  -------------
M2M100 418M model (Meta)          huggingface.co/facebook/m2m100_418M, int8 CT2       MIT
CTranslate2 (OpenNMT / SYSTRAN)   v4.9.1, static, without MKL or OpenMP               MIT
Ruy (Google)                      submodule of CTranslate2 v4.9.1                     Apache 2.0
cpu_features (Google)             submodule of CTranslate2 v4.9.1                     Apache 2.0
spdlog (Gabi Melman)              submodule of CTranslate2 v4.9.1                     MIT
BS::thread_pool, half_float,      headers shipped with CTranslate2 v4.9.1             MIT
nlohmann/json
SentencePiece (Google)            vcpkg x64-windows-static                            Apache 2.0
Abseil (Google)                   vcpkg x64-windows-static (SentencePiece dependency) Apache 2.0
Protocol Buffers lite (Google)    vcpkg x64-windows-static (SentencePiece dependency) BSD 3-Clause
utf8_range                        vcpkg x64-windows-static (Protobuf dependency)      MIT

None of the components is modified. The model is only converted to another format
(PyTorch -> CTranslate2) and quantized to int8.
