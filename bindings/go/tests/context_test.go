/*
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2021. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

package goucxtests

import (
	"testing"
	. "github.com/openucx/ucx/bindings/go/src/ucx"
)

func TestUcpContext(t *testing.T) {
	ucpParams := &UcpParams{}
	ucpParams.SetTagSenderMask(9).EnableStream().SetName("GO_Test").SetEstimatedNumPPN(1)

	context, err := NewUcpContext(ucpParams)

	if err != nil {
		t.Fatalf("Failed to create a context %v", err)
	}

	ucpParams.SetName("Go test2")

	context.Close()
}
