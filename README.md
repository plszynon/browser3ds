# Browser3DS

A thin-client web browser for the original Nintendo 3DS. A Supabase Edge Function
(`browse3ds`, already deployed to project RecMobile) fetches modern pages, strips
them down to text + links and sends them to the 3DS.

URL: https://tiricozrtwjkfobvyfjr.supabase.co/functions/v1/browse3ds
Key (in supabase/functions/browse3ds/index.ts and the app config): xk3ds-7Tq9Lm2Vd8Rp4Wa

## Build
Push to a GitHub repo named `browser3ds`; the Actions workflow produces browser3ds.cia and .3dsx.
Install the .cia with FBI.

## Controls
D-pad / Circle Pad scroll, L/R (or A) page up/down, X/Y next/previous part of a long page,
B back, START exit. Bottom screen: Back, Home, URL, Search, Reload, Bookmark, Exit and the
tappable link list.

## Config
On first start the app writes sdmc:/3ds/browser3ds/config.txt (server= and key=).
Bookmarks are stored in sdmc:/3ds/browser3ds/bookmarks.txt.

## Change the key
Edit KEY in index.ts, redeploy the function, and put the same key= in config.txt.

## Limits
Text and links only (no images, no JavaScript apps), ASCII characters only.
If the 3DS cannot make the HTTPS connection to Supabase, the error code is shown on screen.
