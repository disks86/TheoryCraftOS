/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "Browser/PublicSuffix.hpp"

namespace browser {

namespace {

// Suffixes of two or more labels (registry second levels, then the hosting services). A "*." entry means every name one label below is a suffix as well.
const char* const kSuffixes[] = {
    // United Kingdom, Ireland and the rest of the British domains
    "co.uk", "org.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "sch.uk", "ac.uk", "gov.uk", "nhs.uk", "police.uk", "mod.uk", "com.ie", "gov.ie",
    // Australia, New Zealand
    "com.au", "net.au", "org.au", "edu.au", "gov.au", "asn.au", "id.au", "csiro.au", "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz", "school.nz", "geek.nz", "gen.nz", "kiwi.nz", "maori.nz",
    // Japan, Korea, China, Taiwan, Hong Kong, Singapore
    "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp", "ad.jp", "ed.jp", "gr.jp", "lg.jp", "co.kr", "or.kr", "ne.kr", "go.kr", "re.kr", "pe.kr", "ac.kr", "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "ac.cn",
    "com.tw", "edu.tw", "gov.tw", "net.tw", "org.tw", "idv.tw", "com.hk", "edu.hk", "gov.hk", "net.hk", "org.hk", "idv.hk", "com.sg", "edu.sg", "gov.sg", "net.sg", "org.sg", "per.sg",
    // India and its neighbours
    "co.in", "net.in", "org.in", "firm.in", "gen.in", "ind.in", "ac.in", "edu.in", "res.in", "gov.in", "nic.in", "com.pk", "net.pk", "org.pk", "edu.pk", "gov.pk", "com.bd", "net.bd", "org.bd", "edu.bd", "gov.bd",
    "com.np", "org.np", "edu.np", "gov.np", "com.lk", "org.lk", "edu.lk", "gov.lk",
    // South-East Asia
    "co.id", "web.id", "ac.id", "or.id", "go.id", "sch.id", "my.id", "com.my", "net.my", "org.my", "gov.my", "edu.my", "com.ph", "net.ph", "org.ph", "gov.ph", "edu.ph", "com.vn", "net.vn", "org.vn", "gov.vn", "edu.vn",
    "co.th", "in.th", "ac.th", "go.th", "or.th", "net.th",
    // Middle East and Africa
    "co.il", "org.il", "net.il", "ac.il", "gov.il", "muni.il", "k12.il", "com.tr", "net.tr", "org.tr", "gov.tr", "edu.tr", "gen.tr", "com.sa", "org.sa", "gov.sa", "edu.sa", "com.eg", "org.eg", "gov.eg", "edu.eg",
    "co.za", "org.za", "net.za", "gov.za", "ac.za", "web.za", "co.ke", "or.ke", "go.ke", "ac.ke", "ne.ke", "co.tz", "or.tz", "go.tz", "ac.tz", "com.ng", "org.ng", "gov.ng", "edu.ng", "net.ng", "co.ug", "or.ug", "go.ug", "ac.ug",
    "com.gh", "org.gh", "gov.gh", "edu.gh", "co.zw", "org.zw", "ac.zw", "co.ma", "gov.ma", "ac.ma",
    // The Americas
    "com.br", "net.br", "org.br", "gov.br", "edu.br", "art.br", "eco.br", "eng.br", "tv.br", "com.mx", "org.mx", "gob.mx", "edu.mx", "net.mx", "com.ar", "net.ar", "org.ar", "gob.ar", "edu.ar", "gov.ar",
    "com.co", "net.co", "org.co", "edu.co", "gov.co", "com.pe", "net.pe", "org.pe", "gob.pe", "edu.pe", "com.ve", "net.ve", "org.ve", "gob.ve", "com.uy", "org.uy", "gub.uy", "edu.uy", "com.ec", "gob.ec", "edu.ec",
    "com.cl", "gob.cl", "com.bo", "gob.bo", "com.py", "com.do", "com.gt", "com.sv", "com.ni", "com.pa", "com.cu", "com.pr", "com.jm", "ab.ca", "bc.ca", "mb.ca", "nb.ca", "nf.ca", "nl.ca", "ns.ca", "nt.ca", "nu.ca",
    "on.ca", "pe.ca", "qc.ca", "sk.ca", "yk.ca", "gc.ca",
    // Europe
    "com.ua", "org.ua", "net.ua", "gov.ua", "edu.ua", "kiev.ua", "co.at", "or.at", "ac.at", "gv.at", "com.pl", "net.pl", "org.pl", "gov.pl", "edu.pl", "waw.pl", "com.pt", "org.pt", "gov.pt", "edu.pt",
    "com.es", "org.es", "gob.es", "edu.es", "nom.es", "com.gr", "org.gr", "gov.gr", "edu.gr", "com.ro", "org.ro", "nom.ro", "co.hu", "org.hu", "com.hr", "com.rs", "co.rs", "org.rs", "edu.rs", "com.cy",
    "org.cy", "com.mt", "org.mt", "com.ru", "org.ru", "net.ru", "pp.ru", "com.by", "gov.by", "com.lv", "org.lv", "com.ee", "org.ee", "co.no", "com.fr", "asso.fr", "gouv.fr", "nom.fr", "prd.fr", "tm.fr",
    "com.de", "co.com", "uk.com", "us.com", "eu.com", "de.com", "gb.com", "cn.com", "ru.com", "sa.com", "za.com", "br.com", "jpn.com", "kr.com", "ar.com", "hu.com", "no.com", "qc.com", "se.net", "uk.net",
    // Hosting services (names below these belong to different people)
    "github.io", "githubusercontent.com", "gitlab.io", "bitbucket.io", "herokuapp.com", "herokussl.com", "appspot.com", "blogspot.com", "blogspot.co.uk", "blogspot.de", "blogspot.fr", "cloudfront.net",
    "netlify.app", "netlify.com", "vercel.app", "now.sh", "pages.dev", "workers.dev", "web.app", "firebaseapp.com", "glitch.me", "repl.co", "replit.app", "onrender.com", "fly.dev", "azurewebsites.net",
    "azurestaticapps.net", "azureedge.net", "cloudapp.net", "trafficmanager.net", "s3.amazonaws.com", "elasticbeanstalk.com", "amazonaws.com", "cloudfunctions.net", "run.app", "ngrok.io", "ngrok-free.app",
    "wordpress.com", "tumblr.com", "weebly.com", "wixsite.com", "myshopify.com", "squarespace.com", "webflow.io", "000webhostapp.com", "neocities.org", "surge.sh", "deno.dev", "val.run", "dev.to", "medium.com",
    "pythonanywhere.com", "readthedocs.io", "gitbook.io", "notion.site", "carrd.co", "typedream.app", "bubbleapps.io", "streamlit.app", "hf.space", "codeberg.page", "sourcehut.site", "duckdns.org", "no-ip.org",
    "dyndns.org", "dynu.net", "ddns.net", "hopto.org", "serveo.net", "localtunnel.me", "loca.lt", "trycloudflare.com", "cloudflare-ipfs.com", "ipfs.io", "workers.cloudflare.com",
};

}  // namespace

bool is_public_suffix(StringView domain) {
  if (domain.empty()) return false;
  if (!domain.contains('.')) return true;   // (a top-level domain)
  for (const char* suffix : kSuffixes)
    if (domain.equals(suffix)) return true;
  return false;
}

}  // namespace browser
