package web

import (
	"bytes"
	"encoding/json"
	"encoding/xml"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/i18n"
)

func TestSearchFilesNameOnlyTheCanonicalHomePage(t *testing.T) {
	s := &Server{}

	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/robots.txt", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("robots status = %d", rec.Code)
	}
	if got := rec.Header().Get("Content-Type"); got != "text/plain; charset=utf-8" {
		t.Errorf("robots Content-Type = %q", got)
	}
	if !strings.Contains(rec.Body.String(), "Allow: /\n") ||
		!strings.Contains(rec.Body.String(), "Sitemap: "+officialSiteURL+"sitemap.xml") {
		t.Errorf("robots.txt = %q", rec.Body.String())
	}

	rec = httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/sitemap.xml", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("sitemap status = %d", rec.Code)
	}
	if got := rec.Header().Get("Content-Type"); got != "application/xml; charset=utf-8" {
		t.Errorf("sitemap Content-Type = %q", got)
	}
	var doc struct {
		URLs []struct {
			Location string `xml:"loc"`
		} `xml:"url"`
	}
	if err := xml.Unmarshal(rec.Body.Bytes(), &doc); err != nil {
		t.Fatal(err)
	}
	if len(doc.URLs) != 1 || doc.URLs[0].Location != officialSiteURL {
		t.Errorf("sitemap URLs = %#v", doc.URLs)
	}
}

func TestLandingSEOIsLocalizedAndValidJSON(t *testing.T) {
	seo := landingSEO("ko")
	if seo.Canonical != officialSiteURL || seo.Image != socialImageURL {
		t.Errorf("canonical = %q, image = %q", seo.Canonical, seo.Image)
	}
	if seo.Title != i18n.T("ko", "web.welcome.seo.title") ||
		seo.Description != i18n.T("ko", "web.welcome.seo.desc") {
		t.Errorf("SEO text did not use the page language: %#v", seo)
	}
	var schema websiteSchema
	if err := json.Unmarshal([]byte(seo.StructuredData), &schema); err != nil {
		t.Fatal(err)
	}
	if schema.Type != "WebSite" || schema.Name != "DaeMoon" || schema.URL != officialSiteURL {
		t.Errorf("schema = %#v", schema)
	}
}

func TestOnlyTheLandingDocumentIsIndexable(t *testing.T) {
	s, err := New(nil, config.Default())
	if err != nil {
		t.Fatal(err)
	}

	landing := page{Title: "web.welcome.title", Lang: i18n.Default, Indexable: true}
	landing.SEO = landingSEO(landing.Lang)
	var out bytes.Buffer
	if err := s.tpl.ExecuteTemplate(&out, "welcome.html", landing); err != nil {
		t.Fatal(err)
	}
	html := out.String()
	for _, want := range []string{
		`<link rel="canonical" href="https://daemoon.mir.sh/">`,
		`<meta name="robots" content="index,follow,max-image-preview:large,max-snippet:-1">`,
		`<meta property="og:image" content="https://daemoon.mir.sh/static/social-card.png">`,
		`<script type="application/ld+json">`,
	} {
		if !strings.Contains(html, want) {
			t.Errorf("landing page has no %q", want)
		}
	}

	out.Reset()
	if err := s.tpl.ExecuteTemplate(&out, "login.html",
		page{Title: "web.login.title", Lang: i18n.Default}); err != nil {
		t.Fatal(err)
	}
	html = out.String()
	if !strings.Contains(html, `<meta name="robots" content="noindex,nofollow">`) {
		t.Error("login page is not marked noindex")
	}
	if strings.Contains(html, `rel="canonical"`) {
		t.Error("login page declares a canonical landing page")
	}
}
