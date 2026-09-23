/**
 * @file world_delta.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "world_delta.h"
#include <shared_mutex>
#include <atomic>
#include <mutex>
#include <utility>
#include "../core/log.h"
#include "../core/job_system.h"
#include "../world/block.h"
#include <chrono>
#include <thread>
#include <unordered_map>

namespace save {

using namespace world;

namespace {