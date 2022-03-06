#!/bin/bash

set -ex

dkms_name="spi-i2c-tiny6212"
dkms_version="0.1"

dkms status -m "$dkms_name" -v "$dkms_version" | egrep -q '(added|built|installed)' || dkms add "$(dirname "$0")/src"
dkms status -m "$dkms_name" -v "$dkms_version" | egrep -q '(built|installed)' || dkms build "$dkms_name/$dkms_version"
dkms status -m "$dkms_name" -v "$dkms_version" | egrep -q '(installed)' || dkms install --force "$dkms_name/$dkms_version" "$@"
