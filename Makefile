all:
	gcc -o Server Server.c -lpthread `pkg-config --cflags --libs glib-2.0` -lnetpbm
	gcc -o client client.c -lGLU -lGL -lXext -lX11 -lm -lpthread

clean:
	rm client
	rm Server
