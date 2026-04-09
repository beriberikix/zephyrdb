# ZephyrDB Test Suite

Comprehensive automated test suite for ZephyrDB using Zephyr's native `ztest` framework and `twister` test runner.

## Test Structure

```
tests/
├── unit/                          # Unit tests for isolated APIs
│   ├── kv_basic_test.c           # 10 tests for KV module
│   ├── ts_basic_test.c           # 12 tests for TS module
│   └── doc_basic_test.c          # 10 tests for DOC module
├── integration/                   # Integration tests for multi-module workflows
│   └── workflows_test.c          # 8 tests for common workflows
├── hardware/                      # Hardware-specific persistence tests
│   ├── persistence_nrf52840dk_test.c  # 7 tests for NVS/LittleFS backends
│   └── (future) persistence_fcb_test.c   # FCB backend tests
├── samples/                       # Sample validation tests
│   └── verify_samples_test.c     # 3 tests for sample workflows
├── fixtures/                      # Shared test utilities
│   ├── common.h                  # Test macros and helpers
│   └── backends_mock.c           # Mock NVS/LittleFS implementations
├── CMakeLists.txt                # Test build configuration
├── twister.yaml                  # Twister test scenarios
└── README.md                      # This file
```

## Test Coverage

### Unit Tests (~32 tests)
- **KV Module** (10 tests): Basic CRUD operations, namespace isolation, size limits
- **TS Module** (12 tests): Append, flush, aggregation, cursor iteration, recovery
- **DOC Module** (10 tests): Field operations, persistence, filtering, FlatBuffer export

### Integration Tests (8 tests)
- Module independence (KV + TS simultaneous use)
- Common workflows (mirrors actual samples)
- Health status transitions
- Statistics consistency

### Hardware Tests (7 tests, board-specific)
- **NVS Persistence**: KV data survives reopen cycle
- **LittleFS Append**: TS samples written and readable
- **Recovery**: Corruption detection and recovery
- **FCB Circular Buffer**: Wraparound handling (if available)
- **Health Under Load**: Degradation detection when FS full

### Sample Validation (3 tests)
- `samples/kv_basic` workflow
- `samples/ts_basic` workflow
- `samples/doc_basic` workflow

## Running Tests Locally

### Prerequisites
```bash
# Install Zephyr (if not already installed)
west update

# Initialize Python environment
python -m venv .venv
source .venv/bin/activate  # or .venv\Scripts\activate on Windows
pip install -r $ZEPHYR_BASE/requirements.txt
```

### Run All Tests (native_sim)
```bash
# Unit tests only
west twister -S tests/unit/ -p native_sim -v

# Integration tests
west twister -S tests/integration/ -p native_sim -v

# Sample validation
west twister -S tests/samples/ -p native_sim -v

# All tests combined
west twister -S tests/ -p native_sim -v --tag unit --tag integration --tag samples
```

### Run Hardware Tests
```bash
# Build only (verify compilation)
west twister -S tests/hardware/ -p nrf52840dk_nrf52840 --build-only -v

# Run on connected board (assuming device at /dev/ttyACM0)
west twister -S tests/hardware/ -p nrf52840dk_nrf52840 \
  --device-serial /dev/ttyACM0 -v --timeout 60
```

### Run Specific Test
```bash
# Single test file
west twister -S tests/unit/kv_basic_test.c -p native_sim -v

# Tests matching a tag
west twister -S tests/ -p native_sim --tag kv -v
```

## Test Configuration

### Timeouts
- **Unit tests**: 5 seconds (native_sim is fast)
- **Integration tests**: 10 seconds
- **Hardware tests**: 30–60 seconds (flash I/O is slow)

### Memory Configuration
Test instances use smaller memory pools than production:
- **Core slab**: 16 blocks × 128B = 2KB
- **Cursor slab**: 8 blocks × 96B = 768B
- **KV I/O slab**: 2 blocks × 128B = 256B
- **TS Ingest slab**: 16 blocks × 64B = 1KB
- **Total**: ~4KB per test instance

### Mock Backends
Unit and integration tests use in-memory mocks:
- **Mock NVS**: Simple hash table, 10 KV pairs max
- **Mock LittleFS**: File map, 4KB total storage
- **Mock FCB**: Not yet implemented

Hardware tests use real Zephyr drivers (NVS, LittleFS, FCB).

## Test Helpers

### Macros (in `fixtures/common.h`)

```c
/* Define static slabs for test DB instance */
ZDB_TEST_SLABS_DEFINE(my_db);

/* Declare full test instance with slabs */
ZDB_TEST_INSTANCE_DEFINE(my_db);

/* Assert helpers */
assert_zdb_ok(rc);           // rc == ZDB_OK
assert_zdb_eq(rc, expected); // rc == expected

/* Setup */
int zdb_test_init(zdb_t *db, struct k_work_q *wq);
int zdb_test_cleanup(zdb_t *db);

/* Mock backends */
int mock_nvs_init(struct mock_nvs_fs *);
int mock_lfs_init(struct mock_lfs_fs *);
void mock_nvs_reset(struct mock_nvs_fs *);
void mock_lfs_reset(struct mock_lfs_fs *);
```

## CI/CD Integration

GitHub Actions workflow (`.github/workflows/test.yml`) provides:
- **Automatic testing on push/PR**: Unit + integration tests (~2 min)
- **Scheduled hardware tests**: Nightly on nRF52840dk (with manual override)
- **Build verification**: Ensures hardware tests compile on supported boards

### Triggering Hardware Tests in PR
Add label `hw-test` to PR to trigger hardware test build verification.

## Status Codes Tested

ZephyrDB operations return `zdb_status_t`:
- `ZDB_OK` – Success
- `ZDB_ERR_INVAL` – Invalid argument
- `ZDB_ERR_NOMEM` – Memory allocation failed
- `ZDB_ERR_NOT_FOUND` – Key/document not found
- `ZDB_ERR_IO` – I/O operation failed
- `ZDB_ERR_BUSY` – Resource in use
- `ZDB_ERR_TIMEOUT` – Operation timed out
- `ZDB_ERR_UNSUPPORTED` – Feature not enabled (CONFIG_ZDB_*)
- `ZDB_ERR_CORRUPT` – Data corruption detected
- `ZDB_ERR_INTERNAL` – Internal error

Tests verify correct returns for success and error paths.

## Known Limitations

1. **Power-cycle simulation**: Actual reboot tests deferred (Zephyr ztest doesn't support mid-test reboot)
2. **FCB backend tests**: Requires CONFIG_ZDB_TS_FCB_BACKEND (not universally available)
3. **Stress/performance tests**: Excluded from this phase (require profiling infrastructure)
4. **Multi-instance concurrency**: Requires SMP board or virtualization
5. **Full CRC corruption injection**: Requires hardware-level bit-flip simulation

## Future Enhancements

- [ ] Power-cycle recovery simulation via file corruption injection
- [ ] FCB backend variant tests
- [ ] Stress/load testing with configurable sample rates
- [ ] Coverage report generation (lcov integration)
- [ ] Multi-board test matrix (nrf52840dk, nrf5340dk, qemu variants)
- [ ] Zephyr module CI integration

## Contributing

When adding tests:
1. Follow naming: `test_<module>_<feature>_<expectation>`
2. Use `ztest_unit_test()` and `ztest_suite()` macros
3. Include setup/teardown for resource cleanup
4. Document expected behavior with comments
5. Use `zskip()` for conditional tests (missing CONFIG)
6. Verify no memory leaks in teardown

## References

- [Zephyr Testing Documentation](https://docs.zephyrproject.org/latest/develop/test/index.html)
- [Twister Test Runner](https://docs.zephyrproject.org/latest/develop/test/twister.html)
- [ZephyrDB API](../zephyrdb.h)
