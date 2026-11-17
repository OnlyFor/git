# Helpers shared by the file and smart-HTTP fetch.thin tests.

setup_fetch_thin () {
	test_create_repo thin-source &&
	test_seq 10000 >thin-source/data &&
	git -C thin-source add data &&
	test_tick &&
	git -C thin-source commit -m base &&
	git clone --bare --no-local thin-source thin-seed.git &&
	git -C thin-seed.git repack -ad &&
	git -C thin-seed.git multi-pack-index write --bitmap &&
	git -C thin-seed.git commit-graph write --reachable &&
	thin_base_pack=$(basename "$(echo thin-seed.git/objects/pack/*.pack)") &&
	echo appended >>thin-source/data &&
	git -C thin-source commit -am tip &&
	thin_tip=$(git -C thin-source rev-parse HEAD)
}

check_fetch_thin () {
	thin_url=$1 &&
	thin_protocol=$2 &&
	thin_mode=$3 &&
	client="thin-client-$thin_protocol-$thin_mode.git" &&
	cp -R thin-seed.git "$client" &&
	case "$thin_mode" in
	default)
		set -- ;;
	override)
		git -C "$client" config fetch.thin false &&
		set -- -c fetch.thin=true ;;
	*)
		git -C "$client" config fetch.thin "$thin_mode" &&
		set -- ;;
	esac &&
	# Do not mix the local upload-pack's object reads with the client trace.
	case "$thin_url" in
	http*)
		thin_uploadpack=git-upload-pack ;;
	*)
		thin_uploadpack="unset GIT_TRACE GIT_TRACE_PACKET GIT_TRACE_PACK_ACCESS; git-upload-pack" ;;
	esac &&
	>pack-access &&
	# Isolate delta-base reads from optional full connectivity/collision checks.
	GIT_CONNECTED_NO_OBJECTS=1 GIT_INDEX_PACK_NO_COLLISION_CHECK=1 \
	GIT_TRACE="$PWD/commands" GIT_TRACE_PACKET="$PWD/packets" \
	GIT_TRACE_PACK_ACCESS="$PWD/pack-access" \
	git "$@" -c protocol.version="$thin_protocol" \
		-c fetch.negotiationAlgorithm=skipping \
		-c maintenance.auto=false -c fetch.writeCommitGraph=false \
		-c transfer.unpackLimit=1 -C "$client" fetch \
		--upload-pack="$thin_uploadpack" "$thin_url" \
		refs/heads/main:refs/heads/main &&
	if test "$thin_mode" = false
	then
		test_grep ! "> .*thin-pack" packets &&
		test_grep ! "index-pack.*--fix-thin" commands &&
		test_grep ! "$thin_base_pack" pack-access
	else
		test_grep "> .*thin-pack" packets &&
		test_grep "index-pack.*--fix-thin" commands &&
		test_grep "$thin_base_pack" pack-access
	fi &&
	test "$(git -C "$client" rev-parse HEAD)" = "$thin_tip" &&
	git -C "$client" fsck --full
}
