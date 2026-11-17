#!/bin/sh

test_description='metadata-backed object queries preserve object semantics'

. ./test-lib.sh

GIT_TEST_COMMIT_GRAPH=0
GIT_TEST_MULTI_PACK_INDEX=0
GIT_TEST_MULTI_PACK_INDEX_WRITE_BITMAP=0
GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL=0
export GIT_TEST_COMMIT_GRAPH GIT_TEST_MULTI_PACK_INDEX
export GIT_TEST_MULTI_PACK_INDEX_WRITE_BITMAP GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL

compare_graph_query () {
	git -c core.commitGraph=false "$@" >expect &&
	git -c core.commitGraph=true "$@" >actual &&
	test_cmp expect actual
}

test_expect_success 'setup commits and nested tags with independent dates' '
	test_commit first &&
	git branch first &&
	GIT_COMMITTER_DATE="1800000300 +0900" git tag -a late -m late &&
	test_commit second &&
	GIT_COMMITTER_DATE="1800000100 -0500" git tag -a early -m early &&
	git -c advice.nestedTag=false tag -a nested -m nested late &&
	git pack-refs --all &&
	git repack -ad &&
	git multi-pack-index write --bitmap &&
	git commit-graph write --reachable
'

test_expect_success 'graph fields combined with real object size' '
	compare_graph_query for-each-ref \
		--format="%(refname) %(tree) %(parent) %(numparent) %(objectsize)" refs/heads
'

test_expect_success 'graph-only commit fields' '
	compare_graph_query for-each-ref \
		--format="%(refname) %(tree) %(parent) %(numparent)" refs/heads
'

test_expect_success 'nested tag reports its immediate object and type' '
	compare_graph_query for-each-ref \
		--format="%(refname) %(object) %(type)" refs/tags/nested
'

test_expect_success 'tagger date sorting uses the tag date' '
	compare_graph_query for-each-ref --sort=taggerdate \
		--format="%(refname)" refs/tags/early refs/tags/late
'

test_expect_success 'creator date sorting uses the tag date' '
	compare_graph_query for-each-ref --sort=creatordate \
		--format="%(refname)" refs/tags/early refs/tags/late
'

test_expect_success 'committer date sorting and formatted dates' '
	compare_graph_query for-each-ref --sort=-committerdate \
		--format="%(refname)" refs/heads &&
	compare_graph_query for-each-ref --sort=-committerdate \
		--format="%(refname) %(committerdate:iso8601)" refs/heads
'

test_expect_success 'dereferenced commit fields combined with object size' '
	compare_graph_query for-each-ref \
		--format="%(refname) %(*tree) %(*objectsize)" refs/tags
'

# Also swap tree/blob maps, so accepting the damaged header gives a wrong type.
corrupt_type_bitmap () {
	perl -e '
		local $/;
		my $file = shift;
		open my $fh, "<", $file or die $!;
		my $data = <$fh>;
		my ($hashlen, $fault) = @ARGV;
		my $pos = 12 + $hashlen;
		my @maps;
		for (1..4) {
			my $words = unpack("N", substr($data, $pos + 4, 4));
			my $len = 12 + 8 * $words;
			push @maps, substr($data, $pos, $len);
			$pos += $len;
		}
		my $header = substr($data, 0, 12 + $hashlen);
		if ($fault eq "version") { substr($header, 4, 2) = pack("n", 2); }
		if ($fault eq "flags") { substr($header, 6, 2) = pack("n", 0); }
		if ($fault eq "checksum") { substr($header, 12, $hashlen) = "\0" x $hashlen; }
		print $header, $maps[0], $maps[2], $maps[1], $maps[3], substr($data, $pos);
	' "$1" "$(test_oid rawsz)" "$2"
}

for fault in version flags checksum
do
	test_expect_success PERL "invalid bitmap $fault falls back to object data" '
		tree=$(git rev-parse HEAD^{tree}) &&
		bitmap=$(echo .git/objects/pack/multi-pack-index-*.bitmap) &&
		cp "$bitmap" bitmap.save &&
		test_when_finished "cp bitmap.save \"$bitmap\"" &&
		corrupt_type_bitmap bitmap.save "$fault" >"$bitmap" &&
		echo tree >expect &&
		git cat-file -t "$tree" >actual &&
		test_cmp expect actual
	'
done

test_expect_success 'graph-backed tree lookup still validates the pack header' '
	pack=$(echo .git/objects/pack/pack-*.pack) &&
	cp "$pack" pack.save &&
	test_when_finished "cp pack.save \"$pack\"" &&
	printf BAD! | dd of="$pack" conv=notrunc 2>/dev/null &&
	test_must_fail git show HEAD:first.t
'

test_expect_success 'batch type queries alternate between independent MIDX files' '
	test_create_repo alternate &&
	test_commit -C alternate other &&
	git -C alternate repack -ad &&
	git -C alternate multi-pack-index write --bitmap &&
	blob=$(git rev-parse HEAD:first.t) &&
	tree=$(git -C alternate rev-parse HEAD^{tree}) &&
	{
		printf "%s\n" "$blob" "$tree" "$blob" "$tree"
	} >queries &&
	printf "blob\ntree\nblob\ntree\n" >expect &&
	GIT_ALTERNATE_OBJECT_DIRECTORIES="$PWD/alternate/.git/objects" \
		git cat-file --batch-check="%(objecttype)" <queries >actual &&
	test_cmp expect actual
'

test_done
