#include <benchmark/benchmark.h>

// static void BM_YourBench(benchmark::State& state) {
//     for (auto _ : state) {
//         // 被测代码
//     }
// }
// BENCHMARK(BM_YourBench);

int main(int argc, char** argv) {
    // 你的自定义初始化逻辑
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}