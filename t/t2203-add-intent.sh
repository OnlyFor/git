	test $(git diff --name-only --ita-invisible-in-index HEAD -- nitfol | wc -l) = 0 &&
	test $(git diff --name-only --ita-invisible-in-index -- nitfol | wc -l) = 1
	git diff --cached --name-only >actual &&
	git diff --cached --name-only >actual &&
