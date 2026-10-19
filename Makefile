x86:
	enp_build.sh --static --shared --examples --benchs
x86_install:
	enp_build.sh --static --shared --install

mp135:
	enp_build_mp135.sh
mp135_install:
	enp_build_mp135.sh ~/.local/mp135

clean:
	enp_build.sh --clean
	rm -rf deploy
