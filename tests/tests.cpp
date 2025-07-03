#include <stdio.h>

int main()
{
	for (int i = 0; i < 32; i++)
	{
		printf("%d\n", i & 7);
	}
	return 0;
}