var img_hover_plugins = img_hover_plugins || [];

var img_hover = img_hover || {
	loaded:false,
	imagediv:null,
	imagelink:null,
	image:null,
	spinner:null,
	current_element:null,
	timeout_var:null,
	spinner_data:'data:image/gif;base64,R0lGODlhEgAEAKEAAH9/fwAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQJDwACACwAAAAAEgAEAAACB5SPqcvtrwoAIfkECQ8AAgAsAAAAAAQABAAAAgSEjwkFACH5BAkPAAIALAAAAAALAAQAAAIMjCMJC4fKXBKsnVkLACH5BAkPAAIALAAAAAASAAQAAAIRjCN5mOCwkojt0Xnkg1l1sRUAIfkECQ8AAgAsAAAAABIABAAAAhGEIRkbKRwOUsxBaStdeDdfAAAh+QQJDwACACwHAAAACwAEAAACDIQhGRuHylwSrJ1ZCwAh+QQFDwACACwOAAAABAAEAAACBISPCQUAOw==',

	display_image:function(element, href) {
		img_hover.current_element = element;
		if (img_hover.timeout_var)
			window.clearTimeout(img_hover.timeout_var);
		img_hover.timeout_var = window.setTimeout(function() {
				if (img_hover.image.src !== href || !img_hover.image.complete || img_hover.image.naturalHeight == 0) {
					img_hover.imagelink.style.display = 'none';
					img_hover.spinner.style.display = 'inline';
					if (img_hover.image.src !== href)
						img_hover.image.src = href;
					img_hover.imagelink.href = element.href;
					img_hover.image.style.maxWidth = (window.innerWidth - 22) + 'px';
					img_hover.image.style.maxHeight = (window.innerHeight - 22) + 'px';
				}
				else {
					img_hover.imagelink.style.display = 'inline';
				}
				img_hover.imagediv.style.display = 'inline';
				img_hover.timeout_var = null;
			}, 150);
	},

	document_mouse_move:function(e) {
		if (img_hover.current_element) {
			var rect = img_hover.current_element.getBoundingClientRect();
			if (e.clientY < rect.top || e.clientY > rect.bottom
					|| e.clientX < rect.left || e.clientX > rect.right) {
				if (img_hover.timeout_var) {
					window.clearTimeout(img_hover.timeout_var);
					img_hover.timeout_var = null;
				}
				img_hover.spinner.style.display = 'none';
				img_hover.imagelink.style.display = 'none';
				img_hover.imagediv.style.display = 'none';
				img_hover.current_element = null;
			}
		}
	},

	document_mutation:function(e) {
		if (e.type == 'attributes') {
			if (e.target.tagName === 'A' && e.target !== img_hover.imagelink)
				img_hover.prepare_links(e.target);
		}
		else if (e.type == 'childList') {
			for (var i = 0; i < e.addedNodes.length; ++i) {
				var n = e.addedNodes[i];
				if (n.tagName === 'A')
					img_hover.prepare_links(n);
				else if (n.getElementsByTagName)
					img_hover.prepare_links(n.getElementsByTagName('A'));
			}
		}
	},

	prepare_links:function(links) {
		for (var i = 0; i < img_hover_plugins.length; ++i)
			img_hover_plugins[i].prepare_links(links);
	},

	init:function() {
		img_hover.imagediv = document.createElement('div');
		img_hover.imagelink = document.createElement('a');
		img_hover.image = document.createElement('img');
		img_hover.spinner = document.createElement('img');

		img_hover.imagelink.appendChild(img_hover.image);
		img_hover.imagelink.style.display = 'none';
		img_hover.imagediv.appendChild(img_hover.imagelink);
		img_hover.imagediv.appendChild(img_hover.spinner);

		img_hover.image.style.margin = '0';
		img_hover.image.style.padding = '0';
		img_hover.image.style.border = '1px #aaa solid';
		img_hover.image.style.backgroundColor = '#fff';
		img_hover.image.onload = function() {
			img_hover.spinner.style.display = 'none';
			img_hover.imagelink.style.display = 'inline';
		}

		img_hover.spinner.style.display = 'none';
		img_hover.spinner.style.margin = '0';
		img_hover.spinner.style.padding = '2px';
		img_hover.spinner.style.border = '1px #aaa solid';
		img_hover.spinner.style.backgroundColor = '#fff';
		img_hover.spinner.src = img_hover.spinner_data;

		img_hover.imagediv.style.position = 'fixed';
		img_hover.imagediv.style.top = '10px';
		img_hover.imagediv.style.left = '10px';
		img_hover.imagediv.style.margin = '0';
		img_hover.imagediv.style.padding = '0';
		img_hover.imagediv.style.borderWidth = '0';
		img_hover.imagediv.style.zIndex = 2147483647;
		img_hover.imagediv.overflow = 'hidden';
		img_hover.imagediv.style.display = 'none';

		document.body.appendChild(img_hover.imagediv);

		document.addEventListener('mousemove', img_hover.document_mouse_move, false);
		var observer = new MutationObserver(function(mutations) {
			mutations.forEach(img_hover.document_mutation);
		});
		observer.observe(document.body, { childList:true, attributes:true, subtree:true, attributeFilter:['href'] });
		img_hover.prepare_links(document.getElementsByTagName('A'));
		img_hover.loaded = true;
	},
}

if (!img_hover.loaded)
	img_hover.init();
