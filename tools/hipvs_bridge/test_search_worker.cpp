// Author：FDE WorkMate
// Real-GPU search worker lifecycle / concurrency / shape tests (not a scoring benchmark).
// hipcc -O2 -Itools/hipvs_bridge test_search_worker.cpp -L<bridge-dir>
//   -lseekdb_cuvs_bridge -lpthread -o test_search_worker
// usage: test_search_worker <persisted-index> <rows> <dimension>
#include "seekdb_cuvs_bridge.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static std::atomic<int> failures{0};
static void check(bool ok, const char* message)
{
  if (!ok) { ++failures; fprintf(stderr, "FAIL %s\n", message); }
}
static void query(void* h, long n, long dim, long topk, int seed, bool distances)
{
  std::vector<float> q(dim);
  for (long d=0; d<dim; ++d) q[d] = float((d*13+seed*7)%211)/211.0f;
  std::vector<unsigned> ids(topk);
  std::vector<float> dist(topk);
  int rc = seekdb_cuvs_search(h, q.data(), 1, topk, ids.data(), distances ? dist.data() : nullptr);
  check(rc==0, "single query return code");
  if (rc) return;
  std::vector<unsigned> sorted(ids);
  std::sort(sorted.begin(), sorted.end());
  check(sorted.back() < (unsigned)n, "IDs in range");
  check(std::adjacent_find(sorted.begin(), sorted.end())==sorted.end(), "distinct IDs");
  if (distances) for (float x:dist) check(std::isfinite(x) && x>=0, "valid squared L2 distance");
}
int main(int argc, char** argv)
{
  if (argc!=4) return 2;
  const long n=atol(argv[2]), dim=atol(argv[3]);
  check(seekdb_cuvs_deserialize(argv[1], n+1, dim)==nullptr, "reject snapshot row mismatch");
  const long ks[]={1,10,100,129,256};
  for (int cycle=0; cycle<4; ++cycle) {
    void* h=seekdb_cuvs_deserialize(argv[1], n, dim);
    check(h!=nullptr, "deserialize lifecycle");
    if (!h) break;
    unsigned id=0;
    float q=0;
    check(seekdb_cuvs_search(h, &q, 0, 10, &id, nullptr)!=0, "reject zero queries");
    check(seekdb_cuvs_search(h, nullptr, 1, 10, &id, nullptr)!=0, "reject null query");
    check(seekdb_cuvs_search(h, &q, 1, 0, &id, nullptr)!=0, "reject zero topk");
    check(seekdb_cuvs_search(h, &q, 1, n+1, &id, nullptr)!=0, "reject oversized topk");
    std::vector<std::thread> clients;
    for (int t=0; t<4; ++t) clients.emplace_back([&,t] {
      for (int j=0; j<25; ++j) query(h,n,dim,ks[(j+t)%5],j+t*100,j%2==0);
    });
    for (auto& t:clients) t.join();
    // The non-worker multi-row fallback remains callable after worker initialization.
    std::vector<float> batch(2*dim, 0.25f), distances(20);
    std::vector<unsigned> ids(20);
    check(seekdb_cuvs_search(h,batch.data(),2,10,ids.data(),distances.data())==0, "batch after single queries");
    // free only after clients join: concurrent free/search is not part of the ABI.
    seekdb_cuvs_free(h);
    printf("CYCLE %d complete failures=%d\n",cycle,failures.load());
    fflush(stdout);
  }
  printf("WORKER_STRESS %s failures=%d\n", failures.load()?"FAIL":"PASS",failures.load());
  return failures.load()?1:0;
}
