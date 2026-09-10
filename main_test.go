package main

import (
	"encoding/json"
	"net/http"
	"testing"

	"github.com/router-for-me/CLIProxyAPI/v7/sdk/pluginabi"
)

func TestStatusUsesAuthenticatedManagementRoute(t *testing.T) {
	raw, err := handleMethod(pluginabi.MethodManagementRegister, nil)
	if err != nil {
		t.Fatalf("register management routes: %v", err)
	}

	var env struct {
		OK     bool `json:"ok"`
		Result struct {
			Routes []managementRoute `json:"routes"`
		} `json:"result"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("decode registration response: %v", err)
	}
	if !env.OK {
		t.Fatal("registration response was not successful")
	}
	if len(env.Result.Routes) != 1 {
		t.Fatalf("got %d management routes, want 1", len(env.Result.Routes))
	}

	route := env.Result.Routes[0]
	if route.Method != http.MethodGet {
		t.Errorf("route method = %q, want %q", route.Method, http.MethodGet)
	}
	if route.Path != "/plugins/codex-auto-ping/status" {
		t.Errorf("route path = %q, want authenticated plugin status path", route.Path)
	}

	var result map[string]json.RawMessage
	var outer struct {
		Result json.RawMessage `json:"result"`
	}
	if err := json.Unmarshal(raw, &outer); err != nil {
		t.Fatalf("decode outer response: %v", err)
	}
	if err := json.Unmarshal(outer.Result, &result); err != nil {
		t.Fatalf("decode registration result: %v", err)
	}
	if _, exposed := result["resources"]; exposed {
		t.Fatal("dynamic status must not be exposed through unauthenticated resources")
	}
}
