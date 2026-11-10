#!/bin/bash
# Renders tests/data/images/test96.png with the original Anime4K GLSL shaders
# in mpv and writes the results to tests/data/reference/mpv_Anime4K_*.png.
# They are the references of the test "Anime4K effects match the original
# shaders rendered by mpv": an implementation of the same effects that shares
# no code with Magpie.
#
# Needs mpv, curl and a GPU. The shaders (MIT licence, github.com/bloc97/Anime4K)
# are downloaded to the directory given as the first argument and are not
# part of this repository.
#
# Usage: render_mpv_references.sh DOWNLOAD_DIR
set -eu

download_dir=${1:?usage: render_mpv_references.sh DOWNLOAD_DIR}
tests_dir=$(cd "$(dirname "$0")/.." && pwd)
input=$tests_dir/data/images/test96.png
output_dir=$tests_dir/data/reference
base=https://raw.githubusercontent.com/bloc97/Anime4K/master/glsl

# name in Magpie, path of the original, output width, output height
render() {
	local name=$1 path=$2 width=$3 height=$4
	local shader=$download_dir/$(basename "$path")
	[ -f "$shader" ] || curl -sS -f -L -o "$shader" "$base/${path//+/%2B}"

	# No dithering, so that the values are the shader's own. The output has
	# the size the shader produces, so mpv's own scaler does nothing.
	mpv --no-config --msg-level=all=error --dither-depth=no \
		--glsl-shaders="$shader" "$input" --vf=gpu=w=$width:h=$height \
		--frames=1 -o "$output_dir/mpv_Anime4K_$name.png"
}

mkdir -p "$download_dir" "$output_dir"
render Upscale_S Upscale/Anime4K_Upscale_CNN_x2_S.glsl 192 144
render Upscale_L Upscale/Anime4K_Upscale_CNN_x2_L.glsl 192 144
render Upscale_Denoise_L Upscale+Denoise/Anime4K_Upscale_Denoise_CNN_x2_L.glsl 192 144
render Restore_M Restore/Anime4K_Restore_CNN_M.glsl 96 72
