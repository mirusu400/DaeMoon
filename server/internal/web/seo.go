package web

import (
	"encoding/json"
	"html/template"
	"io"
	"log/slog"
	"net/http"

	"github.com/mirusu400/DaeMoon/server/internal/i18n"
)

// The same landing page is served by every self-hosted instance. Pointing each
// copy back to the project's public home keeps those copies from competing with
// one another in a search index.
const (
	officialSiteURL = "https://daemoon.mir.sh/"
	socialImageURL  = officialSiteURL + "static/social-card.png"
)

type seoData struct {
	Title          string
	Description    string
	Canonical      string
	Image          string
	ImageAlt       string
	StructuredData template.JS
}

type websiteSchema struct {
	Context     string `json:"@context"`
	Type        string `json:"@type"`
	Name        string `json:"name"`
	URL         string `json:"url"`
	Description string `json:"description"`
}

func landingSEO(lang i18n.Lang) seoData {
	description := i18n.T(lang, "web.welcome.seo.desc")
	b, err := json.Marshal(websiteSchema{
		Context:     "https://schema.org",
		Type:        "WebSite",
		Name:        "DaeMoon",
		URL:         officialSiteURL,
		Description: description,
	})
	if err != nil {
		// Every member is a string, so this cannot fail. Keep the check beside the
		// conversion to template.JS: only JSON produced by the encoder is trusted.
		panic(err)
	}
	return seoData{
		Title:          i18n.T(lang, "web.welcome.seo.title"),
		Description:    description,
		Canonical:      officialSiteURL,
		Image:          socialImageURL,
		ImageAlt:       i18n.T(lang, "web.welcome.seo.image"),
		StructuredData: template.JS(string(b)),
	}
}

const robotsText = `User-agent: *
Allow: /

Sitemap: https://daemoon.mir.sh/sitemap.xml
`

func (s *Server) getRobots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	if _, err := io.WriteString(w, robotsText); err != nil {
		slog.DebugContext(r.Context(), "write robots.txt", "err", err)
	}
}

const sitemapXML = `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url><loc>https://daemoon.mir.sh/</loc></url>
</urlset>
`

func (s *Server) getSitemap(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/xml; charset=utf-8")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	if _, err := io.WriteString(w, sitemapXML); err != nil {
		slog.DebugContext(r.Context(), "write sitemap.xml", "err", err)
	}
}
