#!/bin/bash

set -ex

dkms_name="spi-i2c-tiny6212"
dkms_version="0.1"

dkms status -m "$dkms_name" -v "$dkms_version" | egrep -q '(added|built|installed)' &&
	dkms remove "$dkms_name/$dkms_version" --all "$@"
