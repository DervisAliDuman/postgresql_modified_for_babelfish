#!/bin/bash

# Configuration
PREFIX="/home/dervis/postgres_test"
PGDATA="$PREFIX/data"
BIN="$PREFIX/bin"
LIB="$PREFIX/lib"

# Export paths for this execution
export PATH="$BIN:$PATH"
export LD_LIBRARY_PATH="$LIB:$LD_LIBRARY_PATH"

function show_help {
    echo "Usage: $0 {build|clean|init|start|stop|restart|psql|test|status|env|auto-install}"
    echo ""
    echo "Commands:"
    echo "  auto-install - FULL RESET: Stop, clean data, build, init, start, create role"
    echo "  build       - Compile and install the project (including injection_points)"
    echo "  clean       - Clean the build artifacts"
    echo "  init        - Initialize the database cluster (initdb)"
    echo "  create-role - Create the 'postgres' superuser role"
    echo "  start       - Start the PostgreSQL server"
    echo "  stop        - Stop the PostgreSQL server"
    echo "  restart     - Restart the PostgreSQL server"
    echo "  psql [...]  - Run psql (passes arguments through)"
    echo "  test        - Run the injection points regression tests"
    echo "  status      - Check if the server is running"
    echo "  env         - Print environment setup commands"
}

case "$1" in
  build)
    echo "Building and installing..."
    make -j$(nproc) && make install
    # Install injection_points extension
    echo "Installing injection_points extension..."
    make -C src/test/modules/injection_points install
    ;;
  clean)
    echo "Cleaning build..."
    make clean
    ;;
  init)
    echo "Initializing database in $PGDATA..."
    if [ -d "$PGDATA" ]; then
        echo "Data directory $PGDATA already exists."
    else
        initdb -D "$PGDATA"
        # Configure shared_preload_libraries
        echo "shared_preload_libraries = 'injection_points'" >> "$PGDATA/postgresql.conf"
    fi
    ;;
  create-role)
    echo "Creating 'postgres' superuser role..."
    createuser -s postgres
    ;;
  start)
    echo "Starting PostgreSQL..."
    pg_ctl -D "$PGDATA" -l "$PREFIX/logfile" start
    ;;
  stop)
    echo "Stopping PostgreSQL..."
    pg_ctl -D "$PGDATA" stop
    ;;
  restart)
    echo "Restarting PostgreSQL..."
    pg_ctl -D "$PGDATA" stop
    pg_ctl -D "$PGDATA" -l "$PREFIX/logfile" start
    ;;
  status)
    pg_ctl -D "$PGDATA" status
    ;;
  psql)
    shift
    # Check if a database is specified in the arguments
    if [[ "$@" != *"-d "* ]] && [[ "$@" != *"--dbname"* ]]; then
        echo "Connecting to 'postgres' database (default)..."
        psql -d postgres "$@"
    else
        echo "Connecting to PostgreSQL with args: $@"
        psql "$@"
    fi
    ;;
  test)
    echo "Running Injection Points tests..."
    make -C src/test/modules/injection_points check
    ;;
  env)
    echo "export PATH=$BIN:\$PATH"
    echo "export LD_LIBRARY_PATH=$LIB:\$LD_LIBRARY_PATH"
    ;;
  auto-install)
    echo "=== Starting Auto-Install ==="
    
    echo "[1/6] Stopping any running server..."
    pg_ctl -D "$PGDATA" stop >/dev/null 2>&1 || true
    
    echo "[2/6] Cleaning up old data directory..."
    rm -rf "$PGDATA"
    
    echo "[3/6] Building project..."
    $0 build
    
    echo "[4/6] Initializing database..."
    $0 init
    
    echo "[5/6] Starting server..."
    $0 start
    
    echo "[6/6] Creating roles..."
    sleep 2 # Wait for server to be fully ready
    $0 create-role
    
    echo "=== Auto-Install Complete ==="
    ;;
  *)
    show_help
    exit 1
    ;;
esac
