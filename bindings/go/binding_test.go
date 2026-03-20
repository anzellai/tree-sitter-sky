package tree_sitter_sky_test

import (
	"testing"

	tree_sitter "github.com/smacker/go-tree-sitter"
	"github.com/tree-sitter/tree-sitter-sky"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_sky.Language())
	if language == nil {
		t.Errorf("Error loading Sky grammar")
	}
}
