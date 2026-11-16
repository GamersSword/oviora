#pragma once
// SHA-256 of every file in the Ollama v0.40.15 Windows release (ollama-windows-amd64.zip; ollama.exe is
// signed by Ollama Inc.). LocalAi installs only files that match, and the server is started only from a
// folder installed this way (runtime.ok holds OLLAMA_RUNTIME_MARK). A new Ollama version needs a new table.

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QString>

#include <cstring>

inline constexpr const char *OLLAMA_RUNTIME_MARK = "ollama v0.40.15 sha256-verified";

struct OllamaFileHash { const char *name; const char *sha256; };
inline constexpr OllamaFileHash OLLAMA_FILE_HASHES[] = {
    {"lib/ollama/CPP_HTTPLIB_LICENSE", "bb546eae5924d8ee842e0e567e6d533c5824bdebccf605f99a5b3429034e6697"},
    {"lib/ollama/cuda_v12/concrt140.dll", "5d0590c87d33a82265df6bf6683f24d0138a2a68279140b498a24ef27cbd44d1"},
    {"lib/ollama/cuda_v12/cublas64_12.dll", "8bcd86b8e5b3cf5c42c5167c2ac1c816f2302a6b6f451d94388686432edb1492"},
    {"lib/ollama/cuda_v12/cublasLt64_12.dll", "f8c87fad3559ecf0aea0f2794357c5b84171f795e2c3dfe7065156aea2e40840"},
    {"lib/ollama/cuda_v12/cudart64_12.dll", "39306c299af618cedf98a0281174372d45c22a04fcce08e70ef62fdc407a26a9"},
    {"lib/ollama/cuda_v12/ggml-cuda.dll", "45093ee3d1907bd42230f1408da0af1a703b429cd1c7946cce7d8768f916f67f"},
    {"lib/ollama/cuda_v12/msvcp140_1.dll", "d9e16739d7a50bd3caf47bcb27e2e241ce86af2ea0e5f5b228497ada61d02c65"},
    {"lib/ollama/cuda_v12/msvcp140_2.dll", "e4b2e7d3e5a59b50ea5f3220dac21a96c0f72ed2cd85c20897314037ce52aac4"},
    {"lib/ollama/cuda_v12/msvcp140_atomic_wait.dll", "2fa6ad517f9fae15460f0e20f939ddf3dd5aef259043f83facdbed043ecce5bc"},
    {"lib/ollama/cuda_v12/msvcp140_codecvt_ids.dll", "37a96263eb0c17e3583fa04885c3ca26bafdeeab7d0c4a6ee0b0b67d7886fd95"},
    {"lib/ollama/cuda_v12/msvcp140.dll", "0a6229615847ed3462527afd44e7469ebf5d4ace553d7fd354fcc07ba9ed2a5a"},
    {"lib/ollama/cuda_v12/vccorlib140.dll", "2099c06225efc8dc98d45f0cf40b56135bfae61315907c98230ffa3d8718583e"},
    {"lib/ollama/cuda_v12/vcruntime140_1.dll", "0481d322562240b6ff5c042463a8a908d66bd301df405badc12dcd73a13a3c7d"},
    {"lib/ollama/cuda_v12/vcruntime140_threads.dll", "de4121437d8fd572a485aa4122cfa15faf14fef23a631565b56bd6f1a1cb7b98"},
    {"lib/ollama/cuda_v12/vcruntime140.dll", "11eadfa69698f9a7d2567edeb0402f3999792c341ef7475fef0bd02a6b72ab27"},
    {"lib/ollama/cuda_v13/concrt140.dll", "d9314a66ae6efa7a62e1542f1f587216eb942b8bc5fecf28f727c58793d444ad"},
    {"lib/ollama/cuda_v13/cublas64_13.dll", "41f6a35604803e815203822b0721badd97ab7c22d548bf5258f1ff3a91705957"},
    {"lib/ollama/cuda_v13/cublasLt64_13.dll", "4610b6d76604eb357a40b3679f203a8f33fde2e17f44a4e1b5f629908fa0eda5"},
    {"lib/ollama/cuda_v13/cudart64_13.dll", "28fd900f01fb9b9dc0a6e11c84e438ca9d069aa991451e13ec249b4cc00a95ed"},
    {"lib/ollama/cuda_v13/ggml-cuda.dll", "6d166ef20c898178546a941ba8ccd3190bc86c664d43860c7567cd14b0925b6b"},
    {"lib/ollama/cuda_v13/msvcp140_1.dll", "cd8aeea8c13efc43cc4d23899baf9aec433b290096244c5de806f4f1263b44d8"},
    {"lib/ollama/cuda_v13/msvcp140_2.dll", "bbd7b30b61214d23ce903a225821b55087dab147c17aca4e111811260bd2453c"},
    {"lib/ollama/cuda_v13/msvcp140_atomic_wait.dll", "2983823fdda2e74c687b7a87ac516bad5ace9127593f7f2b6a23a93b7fe0ad1a"},
    {"lib/ollama/cuda_v13/msvcp140_codecvt_ids.dll", "41c64e4b2cadf676f896b824c3efe9b87fcb7777d4e4ec52faba69aad75c1449"},
    {"lib/ollama/cuda_v13/msvcp140.dll", "9adf8965e400d8187c7a1550a47b9ffa5c87a2bda1d6da75905cba441857bb30"},
    {"lib/ollama/cuda_v13/nvJitLink_130_0.dll", "0a5f6d916550e2e229b43b151de2197dba9b195a92c4606c99659d855ca822c4"},
    {"lib/ollama/cuda_v13/nvrtc-builtins64_130.dll", "67d887d4d770b510974385029efc01482bad6c90f106b5d7b2d3b6b05f39cc60"},
    {"lib/ollama/cuda_v13/nvrtc64_130_0.alt.dll", "d1fd54f89439b81721ac11f709c86c26faaa7abf610c964fd32e90a4dc03e614"},
    {"lib/ollama/cuda_v13/nvrtc64_130_0.dll", "9e647153e9c516f19da856b33560d5d000b6ad06f9b13e9735ce77c47a8ac82c"},
    {"lib/ollama/cuda_v13/vccorlib140.dll", "1d12f3b3d0cec234e62705987b5c9e762e8fd7c8dfe6b5704baf20c77c347594"},
    {"lib/ollama/cuda_v13/vcruntime140_1.dll", "c745c4c08657f26a0f1a733ca4e48a66eb5d23fb3ec03acc21603dfd2af223cf"},
    {"lib/ollama/cuda_v13/vcruntime140_threads.dll", "73927d7e64ffcf250c86f206757d4a1a12c761a97fd423491b033c028a4eef35"},
    {"lib/ollama/cuda_v13/vcruntime140.dll", "6705e7bd4fe1ebafb55d99a20c415983211f4752c40956f4d01c01f5812f6cfc"},
    {"lib/ollama/ggml-base.dll", "5711ccc197d7e88d6e846cbb9cbede2cb3b058a3f9a34cf6fbd6b369a3171257"},
    {"lib/ollama/ggml-cpu-alderlake.dll", "0653d89c38fca88bcded63b0a07307fb38db25329b964b7a27c86663e316f2b6"},
    {"lib/ollama/ggml-cpu-cannonlake.dll", "2faae0c130d0f338066d23271c5f83b6c10c7aca04a7f13d4e0a2d3a3474707d"},
    {"lib/ollama/ggml-cpu-cascadelake.dll", "963fd21d9742a74dbf95215f8213ba989658400548234dd102d43e697bfaa774"},
    {"lib/ollama/ggml-cpu-cooperlake.dll", "1c1e05560061926b6024cdcdc80d2c9bab2458903c2c673672b98c6e0de577e2"},
    {"lib/ollama/ggml-cpu-haswell.dll", "0eccbcd427c29c1610271e6e53e8659f2e6b96eb0e1706c434571da92f87228a"},
    {"lib/ollama/ggml-cpu-icelake.dll", "edc0adbc33f2b05190837a515d4c37f009ca476ef33e1ec2cf7281b547d1ed49"},
    {"lib/ollama/ggml-cpu-ivybridge.dll", "a69afd3364a03225da04b2d8932ba07cc12a56e5ac5aa38d4da8e951043906f5"},
    {"lib/ollama/ggml-cpu-piledriver.dll", "a905ecf97e5f0748f40066c1c95bbd377b41a84e2652ab82a653c9a7a0c41aa2"},
    {"lib/ollama/ggml-cpu-sandybridge.dll", "d339723ac3e5cc3b3f1199c48fd1f1df8ef3ee4742820d95228ba830237fd1d6"},
    {"lib/ollama/ggml-cpu-sapphirerapids.dll", "b1572792dc4b246e6287e00d42a6a4538d1ea7ef60d98c2c7a6c751bfbdb1cd3"},
    {"lib/ollama/ggml-cpu-skylakex.dll", "faeee67c28b9d5c7164025e43b4a2db699c03d82f268e178318b00d7a9bba8e2"},
    {"lib/ollama/ggml-cpu-sse42.dll", "17564d40973071019bd200a2ea55851854b124b9c5f5d139ba7fe2d7fd282c4a"},
    {"lib/ollama/ggml-cpu-x64.dll", "74a46831c483612decdb6b88685c4a4c5e922a2752528a50949eb88a58679734"},
    {"lib/ollama/ggml-cpu-zen4.dll", "1296b9213925f7af2142eeafa13f355c8e9149e7026a0806300abde6a8c71a29"},
    {"lib/ollama/ggml.dll", "5d78a4cf08580a7fc1548d29982657f505ff28e844e9e80701273322b1955ae2"},
    {"lib/ollama/GO_LICENSE", "4285a95875d7a3ae3790910d59ee88f82339d0ba37b1cbadde4124113bd04308"},
    {"lib/ollama/libc++.dll", "73cc8d15e1b2ed96c6f428d7ac386f394fb227ff8626907b283028a064ac0557"},
    {"lib/ollama/libllama-common.dll", "231882da0a1ac846b0c3dffbb0e18424bafc435c88a0b677cf9df725609093bb"},
    {"lib/ollama/libllama-quantize-impl.dll", "11c6144a5cc4def0c34d83405fcefe8c34efc96b272a594f6b45073f0e8d430b"},
    {"lib/ollama/libllama-server-impl.dll", "252041a7d38b047cf59d6745a89a43817bd7d8e4d7e37ac75fc364631d59466a"},
    {"lib/ollama/libllama.dll", "52b62439f04d1f5ee334f2d7ffb1497470b3be14de813750f56744f133811917"},
    {"lib/ollama/libmtmd.dll", "e123576960ba304493d35f4016a82899288f8eaf04004fc1d5684c24d213f2ef"},
    {"lib/ollama/libomp.dll", "a31c68bcedb7825305ad6ea74195a04ab5cc41a9b4cb684f5d796d1063499d6e"},
    {"lib/ollama/libunwind.dll", "0717c38577aee999a917f316dfe019985a3e97613395a5b9fb222c8320fd5eaa"},
    {"lib/ollama/libwinpthread-1.dll", "dc00c6491c360668fe95a91b6e966570381b66fa2df5260b2da94b1834b2cec6"},
    {"lib/ollama/LLAMA_CPP_LICENSE", "bcd8ec749126d45cb06737d0690295d73df4b6e7e194205bcf91190368f27285"},
    {"lib/ollama/LLAMA_CPP_VENDORS_LICENSE", "7547ae6372a35e7382898dd9f5519ebc16e93095f69c2466b5b74b617c33d4da"},
    {"lib/ollama/llama-quantize.exe", "5ed81310554e85d7f7017fc1c6e272bb6de7b48c07915e38f40a09f9772aec18"},
    {"lib/ollama/llama-server.exe", "4ca54dab7b50c93d6844513a8e7c4d5df7ae277f02b4a97d08e59f09754101f1"},
    {"lib/ollama/vulkan/concrt140.dll", "fec11cdfed3f10ca71ddf1a19f0b3f93418bc264d0ef7854791a87ccad187a89"},
    {"lib/ollama/vulkan/ggml-vulkan.dll", "05c819b6dfc1174acb00f1b5918964dbd6504c2470698d60b047c0f2c0697297"},
    {"lib/ollama/vulkan/msvcp140_1.dll", "8dc1f257dd87c3c718aaddd500ef1a177b0e366885949e3746b168c175fd7f9b"},
    {"lib/ollama/vulkan/msvcp140_2.dll", "cf7dc64f726d6039f265f0c4f58277eef560bb38eb0ad944dab9717bdd2cde99"},
    {"lib/ollama/vulkan/msvcp140_atomic_wait.dll", "6997f8e4937ba7a2fe2d26376025d155a4d69128aaf54e2dc0253e7d00da4b66"},
    {"lib/ollama/vulkan/msvcp140_codecvt_ids.dll", "9c7643ecc658c3d42d0a2bebe70944a487cc6e40693072b8daaf8ff84c72ee72"},
    {"lib/ollama/vulkan/msvcp140.dll", "a2018a0578dd49e11b76d5a853e6cf75ef6ca21b112a46668d264cf3f88ffb33"},
    {"lib/ollama/vulkan/vccorlib140.dll", "6f9ddd784908642ef79bde8b1aa8dbd2ddcd3d341bdc976b477b15a0d135b8d0"},
    {"lib/ollama/vulkan/vcruntime140_1.dll", "e96675dbe820cfca58d3fa45a9eafc00b0b090a66bdf672a268e9baad171888a"},
    {"lib/ollama/vulkan/vcruntime140_threads.dll", "f5cd1f33810fc56afd6c2f5850c1e0c492443c34c3bfaf715d6b3a2d1e0cbae9"},
    {"lib/ollama/vulkan/vcruntime140.dll", "ae5f37c9584e6ec7a12f152044c341f514ebcdcf1489b2db3dcdd404ef0c8341"},
    {"ollama.exe", "544216ca2082b8bc19b4ba94b513f0098fa06774b232f3c140090e3449e322a5"},
};

// The expected hash of a zip entry, empty when the file is not part of the release
inline QByteArray ollamaExpectedHash(const QString &name)
{
    const QByteArray n = name.toUtf8();
    for (const auto &f : OLLAMA_FILE_HASHES)
        if (n == f.name)
            return QByteArray(f.sha256);
    return {};
}

inline bool ollamaFileMatches(const QString &path, const QString &name)
{
    const QByteArray want = ollamaExpectedHash(name);
    QFile f(path);
    if (want.isEmpty() || !f.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex() == want;
}

// A folder LocalAi installed and verified, and its ollama.exe still the release's
inline bool ollamaRuntimeVerified(const QString &dir)
{
    QFile mark(dir + QStringLiteral("/runtime.ok"));
    if (!mark.open(QIODevice::ReadOnly) || mark.readAll().trimmed() != QByteArray(OLLAMA_RUNTIME_MARK))
        return false;
    return ollamaFileMatches(dir + QStringLiteral("/ollama.exe"), QStringLiteral("ollama.exe"));
}