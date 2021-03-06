var img_hover_plugins = img_hover_plugins || [];
img_hover_plugins.push({
	prepare_links:function(links) {
		for (var i = 0; i < links.length; ++i) {
			var matches = links[i].href.match(/^[A-Za-z0-9+.-]+:\/\/(?:www\.)?livememe\.com\/([a-zA-Z0-9]+)/);
			if (matches && matches[1]) {
				var image_url = 'https://e.lvme.me/' + matches[1] + '.jpg';
				links[i].addEventListener('mouseover', {
						element:links[i],
						href:image_url,
						handleEvent:function(event) {
							img_hover.display_image(this.element, this.href);
						}
					}, false);
			}
		}
	}
});
