/**
 * Copyright 2016-2025, XGBoost Contributors
 */
#include <gtest/gtest.h>
#include <xgboost/data.h>
#include <xgboost/host_device_vector.h>  // for HostDeviceVector

#include <filesystem>  // for path
#include <future>      // for future, async
#include <thread>      // for sleep_for

#include "../../../src/common/io.h"
#include "../../../src/data/batch_utils.h"  // for MatchingPageBytes
#include "../../../src/data/sparse_page_dmatrix.h"
#include "../../../src/tree/param.h"  // for TrainParam
#include "../filesystem.h"            // dmlc::TemporaryDirectory
#include "../helpers.h"

using namespace xgboost;  // NOLINT
template <typename Page>
void TestSparseDMatrixLoad(Context const *ctx) {
  auto m = RandomDataGenerator{1024, 5, 0.0}.Batches(4).GenerateSparsePageDMatrix("temp", true);

  auto n_threads = 0;
  auto config = ExtMemConfig{"temp",
                             false,
                             ::xgboost::cuda_impl::AutoHostRatio(),
                             cuda_impl::MatchingPageBytes(),
                             std::numeric_limits<float>::quiet_NaN(),
                             n_threads};
  ASSERT_EQ(AllThreadsForTest(), m->Ctx()->Threads());
  ASSERT_EQ(m->Info().num_col_, 5);
  ASSERT_EQ(m->Info().num_row_, 1024);

  auto simple = RandomDataGenerator{1024, 5, 0.0}.GenerateDMatrix(true);
  Page out;
  for (auto const &page : m->GetBatches<Page>(ctx)) {
    if (std::is_same_v<Page, SparsePage>) {
      out.Push(page);
    } else {
      out.PushCSC(page);
    }
  }
  ASSERT_EQ(m->Info().num_col_, simple->Info().num_col_);
  ASSERT_EQ(m->Info().num_row_, simple->Info().num_row_);
  if (std::is_same_v<Page, SortedCSCPage>) {
    out.SortRows(ctx->Threads());
  }

  for (auto const &page : simple->GetBatches<Page>(ctx)) {
    ASSERT_EQ(page.offset.HostVector(), out.offset.HostVector());
    for (size_t i = 0; i < page.data.Size(); ++i) {
      ASSERT_EQ(page.data.HostVector()[i].fvalue, out.data.HostVector()[i].fvalue);
    }
  }
}

TEST(SparsePageDMatrix, Load) {
  Context ctx;
  TestSparseDMatrixLoad<SparsePage>(&ctx);
  TestSparseDMatrixLoad<CSCPage>(&ctx);
  TestSparseDMatrixLoad<SortedCSCPage>(&ctx);
}

// allow caller to retain pages so they can process multiple pages at the same time.
template <typename Page>
void TestRetainPage() {
  std::size_t n_batches = 4;
  auto p_fmat = RandomDataGenerator{1024, 128, 0.5f}.Batches(n_batches).GenerateSparsePageDMatrix(
      "cache", true);
  Context ctx;
  auto batches = p_fmat->GetBatches<Page>(&ctx);
  auto begin = batches.begin();
  auto end = batches.end();

  std::vector<Page> pages;
  std::vector<std::shared_ptr<Page const>> iterators;
  for (auto it = begin; it != end; ++it) {
    iterators.push_back(it.Page());
    pages.emplace_back(Page{});
    if (std::is_same_v<Page, SparsePage>) {
      pages.back().Push(*it);
    } else {
      pages.back().PushCSC(*it);
    }
    ASSERT_EQ(pages.back().Size(), (*it).Size());
  }
  ASSERT_GE(iterators.size(), n_batches);

  for (size_t i = 0; i < iterators.size(); ++i) {
    ASSERT_EQ((*iterators[i]).Size(), pages.at(i).Size());
    ASSERT_EQ((*iterators[i]).data.HostVector(), pages.at(i).data.HostVector());
  }

  // make sure it's const and the caller can not modify the content of page.
  for (auto &page : p_fmat->GetBatches<Page>({&ctx})) {
    static_assert(std::is_const_v<std::remove_reference_t<decltype(page)>>);
  }
}

TEST(SparsePageDMatrix, RetainSparsePage) {
  TestRetainPage<SparsePage>();
  TestRetainPage<CSCPage>();
  TestRetainPage<SortedCSCPage>();
}

class TestGradientIndexExt : public ::testing::TestWithParam<bool> {
 protected:
  void Run(bool is_dense) {
    constexpr bst_idx_t kRows = 64;
    constexpr size_t kCols = 2;
    float sparsity = is_dense ? 0.0 : 0.4;
    bst_bin_t n_bins = 16;
    Context ctx;
    auto p_ext_fmat =
        RandomDataGenerator{kRows, kCols, sparsity}.Batches(4).GenerateSparsePageDMatrix("temp",
                                                                                         true);

    auto cuts = common::SketchOnDMatrix(&ctx, p_ext_fmat.get(), n_bins, false, {});
    std::vector<std::unique_ptr<GHistIndexMatrix>> pages;
    for (auto const &page : p_ext_fmat->GetBatches<SparsePage>()) {
      pages.emplace_back(std::make_unique<GHistIndexMatrix>(
          page, common::Span<FeatureType const>{}, cuts, n_bins, is_dense, 0.8, ctx.Threads()));
    }
    std::int32_t k = 0;
    for (auto const &page : p_ext_fmat->GetBatches<GHistIndexMatrix>(
             &ctx, BatchParam{n_bins, tree::TrainParam::DftSparseThreshold()})) {
      auto const &from_sparse = pages[k];
      ASSERT_TRUE(std::equal(page.index.begin(), page.index.end(), from_sparse->index.begin()));
      if (is_dense) {
        ASSERT_TRUE(std::equal(page.index.Offset(), page.index.Offset() + kCols,
                               from_sparse->index.Offset()));
      } else {
        ASSERT_FALSE(page.index.Offset());
        ASSERT_FALSE(from_sparse->index.Offset());
      }
      ASSERT_TRUE(
          std::equal(page.row_ptr.cbegin(), page.row_ptr.cend(), from_sparse->row_ptr.cbegin()));
      ++k;
    }
  }
};

TEST_P(TestGradientIndexExt, Basic) { this->Run(this->GetParam()); }

INSTANTIATE_TEST_SUITE_P(SparsePageDMatrix, TestGradientIndexExt, testing::Bool());

// Test GHistIndexMatrix can avoid loading sparse page after the initialization.
TEST(SparsePageDMatrix, GHistIndexSkipSparsePage) {
  dmlc::TemporaryDirectory tmpdir;
  std::size_t n_batches = 6;
  auto Xy = RandomDataGenerator{180, 12, 0.0}.Batches(n_batches).GenerateSparsePageDMatrix(
      tmpdir.path + "/", true);
  Context ctx;
  bst_bin_t n_bins{256};
  double sparse_thresh{0.8};
  BatchParam batch_param{n_bins, sparse_thresh};

  auto check_ghist = [&] {
    std::int32_t k = 0;
    for (auto const &page : Xy->GetBatches<GHistIndexMatrix>(&ctx, batch_param)) {
      ASSERT_EQ(page.Size(), 30);
      ASSERT_EQ(k, page.base_rowid);
      k += page.Size();
    }
  };
  check_ghist();

  auto casted = std::dynamic_pointer_cast<data::SparsePageDMatrix>(Xy);
  CHECK(casted);
  // Make the number of fetches don't change (no new fetch)
  auto n_init_fetches = casted->SparsePageFetchCount();

  std::vector<float> hess(Xy->Info().num_row_, 1.0f);
  // Run multiple iterations to make sure fetches are consistent after reset.
  for (std::int32_t i = 0; i < 4; ++i) {
    auto n_fetches = casted->SparsePageFetchCount();
    check_ghist();
    ASSERT_EQ(casted->SparsePageFetchCount(), n_fetches);
    if (i == 0) {
      ASSERT_EQ(n_fetches, n_init_fetches);
    }
    // Make sure other page types don't interfere the GHist. This way, we can reuse the
    // DMatrix for multiple purposes.
    for ([[maybe_unused]] auto const &page : Xy->GetBatches<SparsePage>(&ctx)) {
    }
    for ([[maybe_unused]] auto const &page : Xy->GetBatches<SortedCSCPage>(&ctx)) {
    }
    for ([[maybe_unused]] auto const &page : Xy->GetBatches<GHistIndexMatrix>(&ctx, batch_param)) {
    }
    // Approx tree method pages
    {
      BatchParam regen{n_bins, common::Span{hess.data(), hess.size()}, false};
      for ([[maybe_unused]] auto const &page : Xy->GetBatches<GHistIndexMatrix>(&ctx, regen)) {
      }
    }
    {
      BatchParam regen{n_bins, common::Span{hess.data(), hess.size()}, true};
      for ([[maybe_unused]] auto const &page : Xy->GetBatches<GHistIndexMatrix>(&ctx, regen)) {
      }
    }
    // Restore the batch parameter by passing it in again through check_ghist
    check_ghist();
  }

  // half the pages
  {
    auto it = Xy->GetBatches<SparsePage>(&ctx).begin();
    for (std::size_t i = 0; i < n_batches / 2; ++i) {
      ++it;
    }
    check_ghist();
  }
  {
    auto it = Xy->GetBatches<GHistIndexMatrix>(&ctx, batch_param).begin();
    for (std::size_t i = 0; i < n_batches / 2; ++i) {
      ++it;
    }
    check_ghist();
  }
  {
    BatchParam regen{n_bins, common::Span{hess.data(), hess.size()}, true};
    auto it = Xy->GetBatches<GHistIndexMatrix>(&ctx, regen).begin();
    for (std::size_t i = 0; i < n_batches / 2; ++i) {
      ++it;
    }
    check_ghist();
  }
}

TEST(SparsePageDMatrix, MetaInfo) {
  dmlc::TemporaryDirectory tmpdir;
  auto dmat = RandomDataGenerator{256, 5, 0.0}.Batches(4).GenerateSparsePageDMatrix(
      tmpdir.path + "/", true);

  // Test the metadata that was parsed
  EXPECT_EQ(dmat->Info().num_row_, 256ul);
  EXPECT_EQ(dmat->Info().num_col_, 5ul);
  EXPECT_EQ(dmat->Info().num_nonzero_, dmat->Info().num_col_ * dmat->Info().num_row_);
  EXPECT_EQ(dmat->Info().labels.Size(), dmat->Info().num_row_);
}

TEST(SparsePageDMatrix, RowAccess) {
  auto dmat = RandomDataGenerator{12, 6, 0.8f}.Batches(2).GenerateSparsePageDMatrix("temp", false);

  // Test the data read into the first row
  auto &batch = *dmat->GetBatches<xgboost::SparsePage>().begin();
  auto page = batch.GetView();
  auto first_row = page[0];
  ASSERT_EQ(first_row.size(), 1ul);
  EXPECT_EQ(first_row[0].index, 5u);
  EXPECT_NEAR(first_row[0].fvalue, 0.1805125, 1e-4);
}

TEST(SparsePageDMatrix, ColAccess) {
  dmlc::TemporaryDirectory tempdir;
  Context ctx;

  auto nan = std::numeric_limits<float>::quiet_NaN();
  HostDeviceVector<float> x{
      0, 10,  20,  nan, nan,  // row-0
      0, nan, nan, 30,  40    // row-1
  };
  auto dmat = GetExternalMemoryDMatrixFromData(x, 2, 5, tempdir, 2);

  // Loop over the batches and assert the data is as expected
  size_t iter = 0;
  for (auto const &col_batch : dmat->GetBatches<xgboost::SortedCSCPage>(&ctx)) {
    auto col_page = col_batch.GetView();
    ASSERT_EQ(col_page.Size(), dmat->Info().num_col_);
    if (iter == 1) {
      ASSERT_EQ(col_page[0][0].fvalue, 0.f);
      ASSERT_EQ(col_page[3][0].fvalue, 30.f);
      ASSERT_EQ(col_page[3][0].index, 1);
      ASSERT_EQ(col_page[3].size(), 1);
    } else {
      ASSERT_EQ(col_page[1][0].fvalue, 10.0f);
      ASSERT_EQ(col_page[1].size(), 1);
    }
    ASSERT_LE(col_batch.base_rowid, dmat->Info().num_row_);
    ++iter;
  }

  // Loop over the batches and assert the data is as expected
  iter = 0;
  for (auto const &col_batch : dmat->GetBatches<xgboost::CSCPage>(&ctx)) {
    auto col_page = col_batch.GetView();
    ASSERT_EQ(col_page.Size(), dmat->Info().num_col_);
    if (iter == 0) {
      ASSERT_EQ(col_page[1][0].fvalue, 10.0f);
      ASSERT_EQ(col_page[1].size(), 1);
    } else {
      ASSERT_EQ(col_page[3][0].fvalue, 30.f);
      ASSERT_EQ(col_page[3].size(), 1);
    }
    iter++;
  }
}

TEST(SparsePageDMatrix, ThreadSafetyException) {
  Context ctx;

  auto dmat =
      RandomDataGenerator{4096, 12, 0.0f}.Batches(8).GenerateSparsePageDMatrix("temp", true);

  int threads = 1000;

  std::vector<std::future<void>> waiting;

  std::atomic<bool> exception {false};

  for (int32_t i = 0; i < threads; ++i) {
    waiting.emplace_back(std::async(std::launch::async, [&]() {
      try {
        auto iter = dmat->GetBatches<SparsePage>().begin();
        ++iter;
      } catch (...) {
        exception.store(true);
      }
    }));
  }

  using namespace std::chrono_literals;

  while (std::any_of(waiting.cbegin(), waiting.cend(), [](auto const &f) {
    return f.wait_for(0ms) != std::future_status::ready;
  })) {
    std::this_thread::sleep_for(50ms);
  }

  CHECK(exception);
}

// Multi-batches access
TEST(SparsePageDMatrix, ColAccessBatches) {
  // Create multiple sparse pages
  auto dmat =
      RandomDataGenerator{1024, 32, 0.4f}.Batches(3).GenerateSparsePageDMatrix("temp", true);
  ASSERT_EQ(dmat->Ctx()->Threads(), AllThreadsForTest());
  Context ctx;
  for (auto const &page : dmat->GetBatches<xgboost::CSCPage>(&ctx)) {
    ASSERT_EQ(dmat->Info().num_col_, page.Size());
  }
}

auto TestSparsePageDMatrixDeterminism(std::int32_t n_threads) {
  std::vector<float> sparse_data;
  std::vector<size_t> sparse_rptr;
  std::vector<bst_feature_t> sparse_cids;

  dmlc::TemporaryDirectory tmpdir;
  auto prefix = (std::filesystem::path{tmpdir.path} / "temp").string();
  auto dmat = RandomDataGenerator{4096, 64, 0.0}.Batches(4).GenerateSparsePageDMatrix(prefix, true);

  auto config = ExtMemConfig{prefix,
                             false,
                             ::xgboost::cuda_impl::AutoHostRatio(),
                             cuda_impl::MatchingPageBytes(),
                             std::numeric_limits<float>::quiet_NaN(),
                             n_threads};
  CHECK(dmat->Ctx()->Threads() == n_threads || dmat->Ctx()->Threads() == AllThreadsForTest());

  DMatrixToCSR(dmat.get(), &sparse_data, &sparse_rptr, &sparse_cids);

  auto cache_name =
      data::MakeId(prefix, dynamic_cast<data::SparsePageDMatrix *>(dmat.get())) + ".row.page";
  auto cache = common::LoadSequentialFile(cache_name);
  return cache;
}

TEST(SparsePageDMatrix, Determinism) {
#if defined(_MSC_VER)
  return;
#endif  // defined(_MSC_VER)
  std::vector<std::vector<char>> caches;
  for (size_t i = 1; i < 18; i += 2) {
    caches.emplace_back(TestSparsePageDMatrixDeterminism(i));
  }

  for (size_t i = 1; i < caches.size(); ++i) {
    ASSERT_EQ(caches[i], caches.front());
  }
}
