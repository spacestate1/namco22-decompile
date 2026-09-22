#!/bin/bash
cd "$(dirname "$0")/build" && ./model_viewer ../extracted/ "$@"
