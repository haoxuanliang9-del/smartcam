#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY="${SCRIPT_DIR}/../third_party"

echo "Downloading third-party libraries..."

echo "  -> concurrentqueue"
if [ ! -f "${THIRD_PARTY}/concurrentqueue/blockingconcurrentqueue.h" ]; then
    curl -sL "https://raw.githubusercontent.com/cameron314/concurrentqueue/master/blockingconcurrentqueue.h" \
        -o "${THIRD_PARTY}/concurrentqueue/blockingconcurrentqueue.h"
    curl -sL "https://raw.githubusercontent.com/cameron314/concurrentqueue/master/concurrentqueue.h" \
        -o "${THIRD_PARTY}/concurrentqueue/concurrentqueue.h"
    echo "    Done"
else
    echo "    Already exists, skipping"
fi

echo "  -> BS_thread_pool"
if [ ! -f "${THIRD_PARTY}/BS_thread_pool/BS_thread_pool.hpp" ]; then
    curl -sL "https://raw.githubusercontent.com/bshoshany/thread-pool/master/include/BS_thread_pool.hpp" \
        -o "${THIRD_PARTY}/BS_thread_pool/BS_thread_pool.hpp"
    echo "    Done"
else
    echo "    Already exists, skipping"
fi

echo "All third-party libraries ready."
