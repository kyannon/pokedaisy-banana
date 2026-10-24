# PokeDaisy for BananaOS - see README.md.
#
#   make            build/pokedaisy for this computer (CMake; needs third_party/mgba)
#   make fixtures   build/fixtures/*.gba: the 3DS port's fixture test ROMs, built with clang + lld
#   make test       the port headless on them: shots in build/shots, the save / state checks
#   make clean
BUILD := build

.PHONY: all fixtures test clean

all:
	cmake -S . -B $(BUILD) -DPOKEDAISY_WERROR=ON
	cmake --build $(BUILD) -j$$(nproc)

fixtures:
	sh test/build_fixture_roms.sh $(abspath $(BUILD))/fixtures

test: all fixtures
	sh test/run.sh $(abspath $(BUILD))/pokedaisy

clean:
	rm -rf $(BUILD)
