# Source this file to set up the prad2hvmon environment (bash/zsh).
#   source <prefix>/bin/setup.sh

PRAD2HV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export PATH="${PRAD2HV_DIR}/bin${PATH:+:$PATH}"

# libcaenhvwrapper.so lives in <prefix>/lib; list lib64 first so RHEL-family
# hosts (GNUInstallDirs default on some distros) also work.
export LD_LIBRARY_PATH="${PRAD2HV_DIR}/lib64:${PRAD2HV_DIR}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

export PRAD2HV_DATABASE_DIR="${PRAD2HV_DIR}/share/prad2hvmon/database"
export PRAD2HV_RESOURCE_DIR="${PRAD2HV_DIR}/share/prad2hvmon/resources"
