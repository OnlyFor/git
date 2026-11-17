#!/bin/sh

test_description='fetch.thin controls external delta-base dependencies'
GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-fetch-thin.sh"

test_expect_success 'setup delta-friendly history and a packed client base' '
	setup_fetch_thin
'

for version in 0 2
do
	for mode in default true false override
	do
		test_expect_success "file protocol v$version fetch.thin=$mode" '
			rm -f commands packets &&
			check_fetch_thin "file://$PWD/thin-source" "$version" "$mode"
		'
	done
done

test_expect_success 'invalid fetch.thin is rejected' '
	test_must_fail git -c fetch.thin=invalid -C thin-seed.git \
		fetch "file://$PWD/thin-source" 2>err &&
	test_grep "bad boolean config value" err
'

test_done
